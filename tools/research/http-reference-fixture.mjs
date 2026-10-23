// Controlled local reference experiment and engine test server. Never serves files from the host.
//
// Routes (all serve the same deterministic payload, byte i = i % 251, unless noted):
//   /range.bin           ranges, ETag, Last-Modified
//   /no-range.bin        "Accept-Ranges: none", ignores Range
//   /redirect            302 to /range.bin
//   /changing.bin        ranges, but every request serves different bytes (overlap mismatch)
//   /versioned.bin       ranges; content, ETag and Last-Modified follow /control/bump
//   /drop.bin?after=N    ranges; every response is cut after N bytes
//   /unknown-length.bin  chunked, no Content-Length, no ranges
//   /gzip.bin, /br.bin   compressed with Content-Encoding although identity was asked for
//   /auth/basic.bin      Basic auth (user "ldm", password "secret")
//   /auth/digest.bin     Digest auth, MD5, qop=auth (same credentials)
//   /disposition.bin     Content-Disposition with filename* (UTF-8) and an unsafe plain name
//   /retry-after.bin     first request answers 429 with Retry-After: 1
//   /flaky.bin           first request answers 503
//   /page.bin            a web page (text/html) where a file is expected
//   /empty.bin           a zero-length file
//   /control/log         JSON: request log and the highest number of concurrent responses
//   /control/bump        changes the content of /versioned.bin
//   /control/reset       clears the log and per-route counters
// An FTP listener on its own port serves the same payload (see ftp-reference-fixture.mjs); the
// ready line reports it as "ftpPort".
import { createHash, randomBytes } from 'node:crypto';
import { createServer } from 'node:http';
import { pathToFileURL } from 'node:url';
import { brotliCompressSync, gzipSync } from 'node:zlib';

import { createFtpFixture } from './ftp-reference-fixture.mjs';

const USER = 'ldm';
const PASSWORD = 'secret';
const REALM = 'ldm-fixture';

function payload(size, salt = 0) {
  const data = Buffer.alloc(size);
  for (let offset = 0; offset < data.length; ++offset) data[offset] = (offset + salt) % 251;
  return data;
}

const sha256 = data => createHash('sha256').update(data).digest('hex');
const md5 = text => createHash('md5').update(text).digest('hex');

function parseDigest(header) {
  const fields = {};
  for (const match of header.slice('Digest '.length).matchAll(/(\w+)=(?:"([^"]*)"|([^,\s]*))/g)) {
    fields[match[1]] = match[2] ?? match[3];
  }
  return fields;
}

export function createReferenceFixture({ size = 16 * 1024 * 1024, delayMs = 40, chunkSize = 32 * 1024, log = () => {} } = {}) {
  if (!Number.isSafeInteger(size) || size < 1 || size > 64 * 1024 * 1024) {
    throw new RangeError('Fixture size must be between 1 byte and 64 MiB');
  }
  if (!Number.isFinite(delayMs) || delayMs < 0) throw new RangeError('Invalid delay');
  if (!Number.isSafeInteger(chunkSize) || chunkSize < 1) throw new RangeError('Invalid chunk size');
  const data = payload(size);
  const digest = sha256(data);
  const compressed = { gzip: gzipSync(data), br: brotliCompressSync(data) };
  const nonce = randomBytes(16).toString('hex');
  let requestId = 0;
  let active = 0;
  let maxActive = 0;
  let version = 0;
  let versioned = data;
  const counters = new Map();
  const entries = [];

  const record = entry => {
    entries.push(entry);
    log(entry);
  };

  // Streams body[start..end] in chunks with a delay, cutting the response after `cutAfter` bytes.
  const stream = (response, body, start, end, id, cutAfter = Infinity) => {
    let offset = start;
    let timer;
    const send = () => {
      if (response.destroyed) return;
      if (offset - start >= cutAfter) { response.destroy(); return; }
      if (offset > end) { response.end(); return; }
      const next = Math.min(offset + chunkSize, end + 1, start + cutAfter);
      const ready = response.write(body.subarray(offset, next));
      offset = next;
      const schedule = () => { timer = setTimeout(send, delayMs); };
      if (ready) schedule();
      else response.once('drain', schedule);
    };
    response.on('close', () => {
      clearTimeout(timer);
      record({ event: 'closed', id, start, end, bytesWritten: offset - start, finished: response.writableFinished });
    });
    send();
  };

  // Answers a request for `body` with range support when `ranges` is set. Returns after headers.
  const serveBody = (request, response, id, body, { ranges = true, etag, lastModified, name, disposition, cutAfter, salt } = {}) => {
    let start = 0;
    let end = body.length - 1;
    const rangeHeader = ranges ? request.headers.range : undefined;
    let ranged = rangeHeader !== undefined;
    if (ranged && request.headers['if-range'] !== undefined && request.headers['if-range'] !== etag &&
        request.headers['if-range'] !== lastModified) {
      ranged = false;  // validator mismatch: the whole current entity, as RFC 9110 requires
    }
    if (ranged) {
      const match = /^bytes=(\d*)-(\d*)$/.exec(rangeHeader);
      if (!match || (!match[1] && !match[2])) {
        response.writeHead(416, { 'Content-Range': `bytes */${body.length}` }).end();
        return;
      }
      if (!match[1]) {
        const suffix = Number(match[2]);
        start = Math.max(0, body.length - suffix);
        if (!Number.isSafeInteger(suffix) || suffix <= 0) start = body.length;
      } else {
        start = Number(match[1]);
        end = match[2] ? Math.min(Number(match[2]), end) : end;
      }
      if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start >= body.length || end < start) {
        response.writeHead(416, { 'Content-Range': `bytes */${body.length}` }).end();
        return;
      }
    }
    const headers = {
      'Content-Type': 'application/octet-stream',
      'Content-Length': end - start + 1,
      'Content-Disposition': disposition ?? `attachment; filename="${name}"`,
      'Accept-Ranges': ranges ? 'bytes' : 'none',
    };
    if (etag) headers.ETag = etag;
    if (lastModified) headers['Last-Modified'] = lastModified;
    if (ranged) headers['Content-Range'] = `bytes ${start}-${end}/${body.length}`;
    response.writeHead(ranged ? 206 : 200, headers);
    if (request.method === 'HEAD') {
      response.end();
      return;
    }
    let source = body;
    if (salt !== undefined) source = payload(body.length, salt);
    stream(response, source, start, end, id, cutAfter);
  };

  const server = createServer((request, response) => {
    const id = ++requestId;
    const url = new URL(request.url, 'http://fixture.invalid');
    const path = url.pathname;
    const count = (counters.get(path) ?? 0) + 1;
    counters.set(path, count);
    record({
      event: 'request', id, method: request.method, path, range: request.headers.range ?? null,
      ifRange: request.headers['if-range'] ?? null, userAgent: request.headers['user-agent'] ?? null,
      referer: request.headers.referer ?? null, acceptEncoding: request.headers['accept-encoding'] ?? null,
    });
    active += 1;
    maxActive = Math.max(maxActive, active);
    response.once('close', () => { active -= 1; });
    const finish = (status, headers = {}, body = '') => {
      response.writeHead(status, headers).end(body);
    };

    if (path.startsWith('/control/')) {
      if (path === '/control/log') {
        finish(200, { 'Content-Type': 'application/json' }, JSON.stringify({ entries, maxActive }));
      } else if (path === '/control/bump') {
        version += 1;
        versioned = payload(size, version * 13);
        finish(204);
      } else if (path === '/control/reset') {
        entries.length = 0;
        counters.clear();
        maxActive = active;
        finish(204);
      } else {
        finish(404);
      }
      return;
    }
    if (!['GET', 'HEAD'].includes(request.method)) {
      finish(405, { Allow: 'GET, HEAD' });
      return;
    }
    const strong = `"${digest}"`;
    const modified = 'Thu, 01 Oct 2026 00:00:00 GMT';
    switch (path) {
      case '/redirect':
        finish(302, { Location: '/range.bin' });
        return;
      case '/range.bin':
        serveBody(request, response, id, data, { etag: strong, lastModified: modified, name: 'range.bin' });
        return;
      case '/no-range.bin':
        serveBody(request, response, id, data, { ranges: false, etag: strong, lastModified: modified, name: 'no-range.bin' });
        return;
      case '/changing.bin':
        serveBody(request, response, id, data, { name: 'changing.bin', salt: id * 7 });
        return;
      case '/versioned.bin':
        serveBody(request, response, id, versioned, {
          etag: `"v${version}"`, lastModified: new Date(Date.UTC(2026, 9, 1 + version)).toUTCString(), name: 'versioned.bin',
        });
        return;
      case '/drop.bin': {
        const after = Number(url.searchParams.get('after') ?? 262144);
        serveBody(request, response, id, data, { etag: strong, lastModified: modified, name: 'drop.bin', cutAfter: after });
        return;
      }
      case '/unknown-length.bin':
        response.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Transfer-Encoding': 'chunked' });
        stream(response, data, 0, data.length - 1, id);
        return;
      case '/gzip.bin':
      case '/br.bin': {
        const encoding = path === '/gzip.bin' ? 'gzip' : 'br';
        const body = compressed[encoding];
        response.writeHead(200, {
          'Content-Type': 'application/octet-stream', 'Content-Encoding': encoding, 'Content-Length': body.length,
        });
        stream(response, body, 0, body.length - 1, id);
        return;
      }
      case '/auth/basic.bin': {
        const expected = `Basic ${Buffer.from(`${USER}:${PASSWORD}`).toString('base64')}`;
        if (request.headers.authorization !== expected) {
          finish(401, { 'WWW-Authenticate': `Basic realm="${REALM}"`, 'Content-Type': 'text/plain' }, 'auth required');
          return;
        }
        serveBody(request, response, id, data, { etag: strong, name: 'basic.bin' });
        return;
      }
      case '/auth/digest.bin': {
        const header = request.headers.authorization ?? '';
        const challenge = `Digest realm="${REALM}", qop="auth", nonce="${nonce}", algorithm=MD5`;
        if (!header.startsWith('Digest ')) {
          finish(401, { 'WWW-Authenticate': challenge, 'Content-Type': 'text/plain' }, 'auth required');
          return;
        }
        const f = parseDigest(header);
        const ha1 = md5(`${USER}:${REALM}:${PASSWORD}`);
        const ha2 = md5(`${request.method}:${f.uri}`);
        const valid = f.username === USER && f.nonce === nonce &&
          f.response === md5(`${ha1}:${nonce}:${f.nc}:${f.cnonce}:${f.qop}:${ha2}`);
        if (!valid) {
          finish(401, { 'WWW-Authenticate': challenge, 'Content-Type': 'text/plain' }, 'auth required');
          return;
        }
        serveBody(request, response, id, data, { etag: strong, name: 'digest.bin' });
        return;
      }
      case '/disposition.bin':
        serveBody(request, response, id, data, {
          etag: strong,
          disposition: `attachment; filename="..\\\\evil:name?.bin"; filename*=UTF-8''%E2%82%AC%20rates%3A%20Q3.bin`,
        });
        return;
      case '/retry-after.bin':
        if (count === 1) {
          finish(429, { 'Retry-After': '1', 'Content-Type': 'text/plain' }, 'slow down');
          return;
        }
        serveBody(request, response, id, data, { etag: strong, name: 'retry-after.bin' });
        return;
      case '/flaky.bin':
        if (count === 1) {
          finish(503, { 'Content-Type': 'text/plain' }, 'unavailable');
          return;
        }
        serveBody(request, response, id, data, { etag: strong, name: 'flaky.bin' });
        return;
      case '/page.bin':
        finish(200, { 'Content-Type': 'text/html; charset=utf-8' }, '<!doctype html><title>Login</title><p>Please sign in</p>');
        return;
      case '/empty.bin':
        finish(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': 0 });
        return;
      default:
        finish(404);
    }
  });
  const ftp = createFtpFixture({ data, delayMs, chunkSize, record, nextId: () => ++requestId });
  return { server, ftpServer: ftp.server, sha256: digest, size };
}

function parseArguments(argv) {
  const options = {};
  for (let i = 0; i < argv.length; ++i) {
    const name = argv[i];
    const value = argv[i + 1];
    if (name === '--size') { options.size = Number(value); ++i; }
    else if (name === '--delay') { options.delayMs = Number(value); ++i; }
    else if (name === '--chunk') { options.chunkSize = Number(value); ++i; }
    else if (name === '--quiet') { options.quiet = true; }
    else throw new Error(`Unknown argument: ${name}`);
  }
  return options;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const { quiet, ...options } = parseArguments(process.argv.slice(2));
  const fixture = createReferenceFixture({
    ...options, log: quiet ? () => {} : event => process.stdout.write(`${JSON.stringify(event)}\n`),
  });
  fixture.server.listen(0, '127.0.0.1', () => {
    fixture.ftpServer.listen(0, '127.0.0.1', () => {
      process.stdout.write(`${JSON.stringify({
        event: 'ready', port: fixture.server.address().port, ftpPort: fixture.ftpServer.address().port,
        size: fixture.size, sha256: fixture.sha256,
      })}\n`);
    });
  });
  const stop = () => { fixture.server.close(); fixture.server.closeAllConnections(); fixture.ftpServer.close(); };
  process.once('SIGINT', stop);
  process.once('SIGTERM', stop);
  // A test harness that dies leaves stdin closed; exit with it instead of lingering.
  if (quiet) {
    process.stdin.resume();
    process.stdin.once('end', () => { stop(); process.exit(0); });
  }
  fixture.server.on('error', error => { process.stderr.write(`${error.message}\n`); process.exitCode = 1; });
}

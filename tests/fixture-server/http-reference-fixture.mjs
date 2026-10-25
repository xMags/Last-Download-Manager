// Controlled local reference experiment and engine test server. Serves files from the host only
// under /media/, from the folder given with --media-root, and nothing when it is not given.
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
//   /site/...            a small web site for the Site Grabber: pages with relative, absolute,
//                        cyclic and parent-folder links, a script link, a picture, files of
//                        several sizes, a file on another host (localhost instead of 127.0.0.1)
//                        and a file behind Basic auth; /outside.html lies above it
//   /large.bin           ranges; --large-size bytes (default 2 GiB) of the same pattern, generated
//                        on the fly, for stress tests
//   /media/<path>        a file under --media-root, with ranges: generated HLS and DASH fixtures
//   /media-gone/<path>   404, for a segment that disappeared
//   /media-live/<path>   the plain MPEG-TS media playlist at /media/<path> as a live stream: a
//                        window of three segments that moves on by one each second from the
//                        route's first request, ending with EXT-X-ENDLIST once the last segment
//                        is in it; its segment addresses point at /media/
//   /releases/latest     a GitHub "latest release" answer for the update check; ?tag=vX.Y.Z picks
//                        the tag (default v5.0.0)
//   /control/log         JSON: request log and the highest number of concurrent responses
//   /control/bump        changes the content of /versioned.bin
//   /control/reset       clears the log and per-route counters
// File routes with an ETag or Last-Modified answer If-None-Match and If-Modified-Since with 304.
// An FTP listener on its own port serves the same payload (see ftp-reference-fixture.mjs); the
// ready line reports it as "ftpPort".
import { createHash, randomBytes } from 'node:crypto';
import { readFileSync, realpathSync, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, isAbsolute, relative, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { brotliCompressSync, gzipSync } from 'node:zlib';

import { createFtpFixture } from './ftp-reference-fixture.mjs';

const USER = 'ldm';
const PASSWORD = 'secret';
const REALM = 'ldm-fixture';

// Looks like a Buffer to serveBody and stream, but holds one repeating block: byte i is i % 251 for
// any length, without allocating it.
function patternBody(length, chunkSize) {
  const block = payload(251 * (Math.ceil(chunkSize / 251) + 1));
  return {
    length,
    subarray(start, end) {
      const from = start % 251;
      if (end - start <= block.length - from) return block.subarray(from, from + end - start);
      const out = Buffer.alloc(end - start);
      for (let i = 0; i < out.length; ++i) out[i] = (start + i) % 251;
      return out;
    },
  };
}

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

// The Site Grabber's fixture site: page path -> HTML, with PORT replaced by the server's port.
const SITE_PAGES = {
  '/site/': `<!doctype html><title>Fixture site</title>
<p><a href="a.html">Page A</a> <a href="/site/b/b.html">Page B</a> <a href="../outside.html">Outside</a>
<a href="./">Again</a> <a href="javascript:void(0)">Script</a></p>
<p><img src="img/pic.png" alt="Picture"></p>
<p><a href="files/small.bin">Small file</a> <a href="files/large.bin">Large file</a>
<a href="http://localhost:PORT/site/files/other.bin">Other host</a> <a href="private/secret.bin">Secret</a></p>`,
  '/site/a.html': '<!doctype html><a href="deep/c.html">Deep</a><img src="img/pic.png"><a href="files/a.bin">A file</a>',
  '/site/b/b.html': '<!doctype html><a href="../files/b.bin">B file</a><a href="../a.html">Back to A</a>',
  '/site/deep/c.html': '<!doctype html><a href="../files/deep.bin">Deep file</a>',
  '/outside.html': '<!doctype html><a href="/site/files/outside.bin">Outside file</a>',
};
// The site's files: path -> [size, content type].
const SITE_FILES = {
  '/site/img/pic.png': [200, 'image/png'],
  '/site/files/small.bin': [100, 'application/octet-stream'],
  '/site/files/large.bin': [5000, 'application/octet-stream'],
  '/site/files/other.bin': [100, 'application/octet-stream'],
  '/site/files/a.bin': [500, 'application/octet-stream'],
  '/site/files/b.bin': [700, 'application/octet-stream'],
  '/site/files/deep.bin': [400, 'application/octet-stream'],
  '/site/files/outside.bin': [100, 'application/octet-stream'],
  '/site/private/secret.bin': [300, 'application/octet-stream'],
};

const MEDIA_TYPES = {
  '.m3u8': 'application/vnd.apple.mpegurl', '.mpd': 'application/dash+xml', '.ts': 'video/mp2t',
  '.m4s': 'video/iso.segment', '.mp4': 'video/mp4', '.m4a': 'audio/mp4', '.vtt': 'text/vtt',
  '.key': 'application/octet-stream', '.bin': 'application/octet-stream',
};

const LIVE_WINDOW = 3;

// A VOD media playlist's text as a live playlist `elapsedMs` after the stream started (see
// /media-live/). `folder` is the playlist's folder under /media/. Plain playlists only: segments
// with EXTINF and their address, no keys or init sections.
function livePlaylist(text, folder, elapsedMs) {
  const header = [];
  const segments = [];
  let pending = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || /^#EXT-X-(MEDIA-SEQUENCE|ENDLIST|PLAYLIST-TYPE)/.test(line)) continue;
    if (!line.startsWith('#')) {
      segments.push([...pending, `/media/${folder}${line}`]);
      pending = [];
    } else if (line.startsWith('#EXTINF') || segments.length > 0 || pending.length > 0) {
      pending.push(line);
    } else {
      header.push(line);
    }
  }
  const end = Math.min(segments.length, LIVE_WINDOW + Math.floor(elapsedMs / 1000));
  const start = Math.max(0, end - LIVE_WINDOW);
  const lines = [...header, `#EXT-X-MEDIA-SEQUENCE:${start}`];
  for (const segment of segments.slice(start, end)) lines.push(...segment);
  if (end === segments.length) lines.push('#EXT-X-ENDLIST');
  return lines.join('\n') + '\n';
}

// The file under `root` that `relativePath` names, or nothing when it leaves the folder or is not
// a file.
function mediaFile(root, relativePath) {
  if (!root) return undefined;
  let decoded;
  try { decoded = decodeURIComponent(relativePath); } catch { return undefined; }
  const base = realpathSync(root);
  const candidate = resolve(base, decoded);
  const inside = relative(base, candidate);
  if (!inside || inside.startsWith('..') || isAbsolute(inside)) return undefined;
  try {
    const real = realpathSync(candidate);
    const within = relative(base, real);
    if (!within || within.startsWith('..') || isAbsolute(within) || !statSync(real).isFile()) return undefined;
    return real;
  } catch {
    return undefined;
  }
}

export function createReferenceFixture({
  size = 16 * 1024 * 1024, delayMs = 40, chunkSize = 32 * 1024, largeSize = 2 * 1024 * 1024 * 1024, mediaRoot,
  log = () => {},
} = {}) {
  if (!Number.isSafeInteger(size) || size < 1 || size > 64 * 1024 * 1024) {
    throw new RangeError('Fixture size must be between 1 byte and 64 MiB');
  }
  if (!Number.isFinite(delayMs) || delayMs < 0) throw new RangeError('Invalid delay');
  if (!Number.isSafeInteger(chunkSize) || chunkSize < 1) throw new RangeError('Invalid chunk size');
  if (!Number.isSafeInteger(largeSize) || largeSize < 1) throw new RangeError('Invalid large size');
  const large = patternBody(largeSize, chunkSize);
  const data = payload(size);
  const digest = sha256(data);
  const compressed = { gzip: gzipSync(data), br: brotliCompressSync(data) };
  const nonce = randomBytes(16).toString('hex');
  let requestId = 0;
  let active = 0;
  let limitedActive = 0;  // requests of /limited.bin under way
  let maxActive = 0;
  let version = 0;
  let versioned = data;
  const counters = new Map();
  const entries = [];
  const liveStarts = new Map();  // /media-live/ path -> when it was first asked for

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
  const serveBody = (request, response, id, body, { ranges = true, etag, lastModified, name, disposition, cutAfter, salt, contentType } = {}) => {
    // Conditional requests (RFC 9110 13.1): If-None-Match decides when present, else If-Modified-Since.
    const noneMatch = request.headers['if-none-match'];
    const modifiedSince = request.headers['if-modified-since'];
    const notModified = noneMatch !== undefined
      ? etag !== undefined && noneMatch.split(',').map((tag) => tag.trim().replace(/^W\//, '')).some((tag) => tag === '*' || tag === etag)
      : modifiedSince !== undefined && lastModified !== undefined && Date.parse(lastModified) <= Date.parse(modifiedSince);
    if (notModified && (request.method === 'GET' || request.method === 'HEAD')) {
      const unchanged = {};
      if (etag) unchanged.ETag = etag;
      if (lastModified) unchanged['Last-Modified'] = lastModified;
      response.writeHead(304, unchanged).end();
      return;
    }
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
      'Content-Type': contentType ?? 'application/octet-stream',
      'Content-Length': end - start + 1,
      'Accept-Ranges': ranges ? 'bytes' : 'none',
    };
    if (!contentType) headers['Content-Disposition'] = disposition ?? `attachment; filename="${name}"`;
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
      streamKey: request.headers['x-stream-key'] ?? null,
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
        liveStarts.clear();
        maxActive = active;
        finish(204);
      } else {
        finish(404);
      }
      return;
    }
    if (path === '/post.bin') {
      // A download that answers a form: only the right form, sent as a POST, gets the file.
      const chunks = [];
      request.on('data', (chunk) => chunks.push(chunk));
      request.on('end', () => {
        const body = Buffer.concat(chunks).toString('utf8');
        if (request.method !== 'POST' || request.headers['content-type'] !== 'application/x-www-form-urlencoded' ||
            body !== 'token=abc&x=1') {
          finish(403, { 'Content-Type': 'text/plain' }, 'form required');
          return;
        }
        serveBody(request, response, id, data, { name: 'post.bin' });
      });
      return;
    }
    if (!['GET', 'HEAD'].includes(request.method)) {
      finish(405, { Allow: 'GET, HEAD' });
      return;
    }
    const strong = `"${digest}"`;
    const modified = 'Thu, 01 Oct 2026 00:00:00 GMT';
    if (path.startsWith('/media/')) {
      const file = mediaFile(mediaRoot, path.slice('/media/'.length));
      if (!file) {
        finish(404);
        return;
      }
      const type = MEDIA_TYPES[extname(file).toLowerCase()] ?? 'application/octet-stream';
      serveBody(request, response, id, readFileSync(file), { contentType: type });
      return;
    }
    if (path === '/releases/latest') {
      const tag = url.searchParams.get('tag') ?? 'v5.0.0';
      finish(200, { 'Content-Type': 'application/json' }, JSON.stringify({
        tag_name: tag, html_url: `https://github.com/xMags/Last-Download-Manager/releases/tag/${tag}`,
        draft: false, prerelease: tag.includes('-'), assets: [],
      }));
      return;
    }
    if (SITE_PAGES[path] !== undefined) {
      const port = (request.headers.host ?? '').split(':').pop();
      finish(200, { 'Content-Type': 'text/html; charset=utf-8' }, SITE_PAGES[path].replaceAll('PORT', port));
      return;
    }
    if (SITE_FILES[path] !== undefined) {
      if (path.startsWith('/site/private/')) {
        const expected = `Basic ${Buffer.from(`${USER}:${PASSWORD}`).toString('base64')}`;
        if (request.headers.authorization !== expected) {
          finish(401, { 'WWW-Authenticate': `Basic realm="${REALM}"`, 'Content-Type': 'text/plain' }, 'auth required');
          return;
        }
      }
      const [size, type] = SITE_FILES[path];
      serveBody(request, response, id, payload(size), { contentType: type });
      return;
    }
    if (path.startsWith('/media-gone/')) {
      finish(404);
      return;
    }
    if (path.startsWith('/media-live/')) {
      const relativePath = path.slice('/media-live/'.length);
      const file = mediaFile(mediaRoot, relativePath);
      if (!file || extname(file).toLowerCase() !== '.m3u8') {
        finish(404);
        return;
      }
      if (!liveStarts.has(path)) liveStarts.set(path, Date.now());
      const folder = relativePath.slice(0, relativePath.lastIndexOf('/') + 1);
      const text = livePlaylist(readFileSync(file, 'utf8'), folder, Date.now() - liveStarts.get(path));
      finish(200, { 'Content-Type': MEDIA_TYPES['.m3u8'], 'Cache-Control': 'no-store' }, text);
      return;
    }
    switch (path) {
      case '/redirect':
        finish(302, { Location: '/range.bin' });
        return;
      case '/range.bin':
        serveBody(request, response, id, data, { etag: strong, lastModified: modified, name: 'range.bin' });
        return;
      case '/large.bin':
        serveBody(request, response, id, large, { etag: '"large"', lastModified: modified, name: 'large.bin' });
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
      case '/cookie.bin': {
        // Browser capture: every connection must carry the session cookie, and a cookie that
        // belongs to another site must never arrive.
        const cookie = request.headers.cookie ?? '';
        if (!cookie.split(/;\s*/).includes('sid=abc') || cookie.includes('leak=')) {
          finish(403, { 'Content-Type': 'text/plain' }, 'cookie required');
          return;
        }
        serveBody(request, response, id, data, { etag: strong, lastModified: modified, name: 'cookie.bin' });
        return;
      }
      case '/limited.bin': {
        // A file server that takes two connections per client and refuses more with 404, as
        // BitChute's do.
        if (limitedActive >= 2) {
          record({ event: 'refused', id, path });
          finish(404, { 'Content-Type': 'text/plain' }, 'too many connections');
          return;
        }
        limitedActive += 1;
        response.once('close', () => { limitedActive -= 1; });
        serveBody(request, response, id, data, { etag: strong, lastModified: modified, name: 'limited.bin' });
        return;
      }
      case '/site-header.bin': {
        // A video site's own header (a stream key), wanted with every request.
        if (request.headers['x-stream-key'] !== 'k1') {
          finish(403, { 'Content-Type': 'text/plain' }, 'stream key required');
          return;
        }
        serveBody(request, response, id, data, { etag: strong, lastModified: modified, name: 'site-header.bin' });
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
    else if (name === '--large-size') { options.largeSize = Number(value); ++i; }
    else if (name === '--media-root') { options.mediaRoot = value; ++i; }
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

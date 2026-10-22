// Controlled local reference experiment. Never serves files from the host.
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { pathToFileURL } from 'node:url';

export function createReferenceFixture({ size = 16 * 1024 * 1024, delayMs = 40, log = () => {} } = {}) {
  if (!Number.isSafeInteger(size) || size < 1 || size > 64 * 1024 * 1024) {
    throw new RangeError('Fixture size must be between 1 byte and 64 MiB');
  }
  if (!Number.isFinite(delayMs) || delayMs < 0) throw new RangeError('Invalid delay');
  const data = Buffer.alloc(size);
  for (let offset = 0; offset < data.length; ++offset) data[offset] = offset % 251;
  const sha256 = createHash('sha256').update(data).digest('hex');
  let requestId = 0;
  const server = createServer((request, response) => {
    const id = ++requestId;
    const path = request.url;
    log({ event: 'request', id, method: request.method, path, range: request.headers.range ?? null });
    if (!['GET', 'HEAD'].includes(request.method)) {
      response.writeHead(405, { Allow: 'GET, HEAD' }).end();
      return;
    }
    if (path === '/redirect') {
      response.writeHead(302, { Location: '/range.bin' }).end();
      return;
    }
    if (!['/range.bin', '/no-range.bin'].includes(path)) {
      response.writeHead(404).end();
      return;
    }
    let start = 0;
    let end = data.length - 1;
    const ranged = path === '/range.bin' && request.headers.range !== undefined;
    if (ranged) {
      const match = /^bytes=(\d*)-(\d*)$/.exec(request.headers.range);
      if (!match || (!match[1] && !match[2])) {
        response.writeHead(416, { 'Content-Range': `bytes */${data.length}` }).end();
        return;
      }
      if (!match[1]) {
        const suffix = Number(match[2]);
        start = Math.max(0, data.length - suffix);
        if (!Number.isSafeInteger(suffix) || suffix <= 0) start = data.length;
      } else {
        start = Number(match[1]);
        end = match[2] ? Math.min(Number(match[2]), end) : end;
      }
      if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start >= data.length || end < start) {
        response.writeHead(416, { 'Content-Range': `bytes */${data.length}` }).end();
        return;
      }
    }
    const headers = {
      'Content-Type': 'application/octet-stream',
      'Content-Length': end - start + 1,
      'Content-Disposition': `attachment; filename="${path.slice(1)}"`,
      'Accept-Ranges': path === '/range.bin' ? 'bytes' : 'none',
      ETag: `"${sha256}"`,
      'Last-Modified': 'Thu, 01 Oct 2026 00:00:00 GMT',
    };
    if (ranged) headers['Content-Range'] = `bytes ${start}-${end}/${data.length}`;
    response.writeHead(ranged ? 206 : 200, headers);
    if (request.method === 'HEAD') {
      response.end();
      return;
    }
    let offset = start;
    let timer;
    const send = () => {
      if (response.destroyed) return;
      if (offset > end) { response.end(); return; }
      const next = Math.min(offset + 32 * 1024, end + 1);
      const ready = response.write(data.subarray(offset, next));
      offset = next;
      const schedule = () => { timer = setTimeout(send, delayMs); };
      if (ready) schedule();
      else response.once('drain', schedule);
    };
    response.on('close', () => {
      clearTimeout(timer);
      log({ event: 'closed', id, start, end, bytesWritten: offset - start, finished: response.writableFinished });
    });
    send();
  });
  return { server, sha256, size };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const fixture = createReferenceFixture({ log: event => process.stdout.write(`${JSON.stringify(event)}\n`) });
  fixture.server.listen(0, '127.0.0.1', () => {
    process.stdout.write(`${JSON.stringify({ event: 'ready', port: fixture.server.address().port, size: fixture.size, sha256: fixture.sha256 })}\n`);
  });
  const stop = () => { fixture.server.close(); fixture.server.closeAllConnections(); };
  process.once('SIGINT', stop);
  process.once('SIGTERM', stop);
  fixture.server.on('error', error => { process.stderr.write(`${error.message}\n`); process.exitCode = 1; });
}

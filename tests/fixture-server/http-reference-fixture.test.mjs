import assert from 'node:assert/strict';
import { once } from 'node:events';
import { createHash } from 'node:crypto';
import test from 'node:test';
import { createReferenceFixture } from './http-reference-fixture.mjs';

test('controlled HTTP reference has correct ranges, fallback and content identity', async () => {
  const fixture = createReferenceFixture({ size: 1024, delayMs: 0 });
  fixture.server.listen(0, '127.0.0.1');
  await once(fixture.server, 'listening');
  const base = `http://127.0.0.1:${fixture.server.address().port}`;
  try {
    const complete = await fetch(`${base}/range.bin`);
    const bytes = Buffer.from(await complete.arrayBuffer());
    assert.equal(createHash('sha256').update(bytes).digest('hex'), fixture.sha256);
    for (const [range, start, end] of [['bytes=100-199', 100, 199], ['bytes=1000-', 1000, 1023], ['bytes=-10', 1014, 1023]]) {
      const result = await fetch(`${base}/range.bin`, { headers: { Range: range } });
      assert.equal(result.status, 206);
      assert.equal(result.headers.get('content-range'), `bytes ${start}-${end}/1024`);
      assert.deepEqual(Buffer.from(await result.arrayBuffer()), bytes.subarray(start, end + 1));
    }
    for (const range of ['bytes=1024-', 'bytes=200-100', 'bytes=-0', 'bytes=-', 'bytes=0-1,3-4', 'bytes=999999999999999999999-']) {
      const result = await fetch(`${base}/range.bin`, { headers: { Range: range } });
      assert.equal(result.status, 416);
      assert.equal(result.headers.get('content-range'), 'bytes */1024');
      await result.arrayBuffer();
    }
    const fallback = await fetch(`${base}/no-range.bin`, { headers: { Range: 'bytes=100-' } });
    assert.equal(fallback.status, 200);
    assert.deepEqual(Buffer.from(await fallback.arrayBuffer()), bytes);
    const redirect = await fetch(`${base}/redirect`);
    assert.deepEqual(Buffer.from(await redirect.arrayBuffer()), bytes);
    const head = await fetch(`${base}/range.bin`, { method: 'HEAD' });
    assert.equal(head.headers.get('content-length'), '1024');
    assert.equal((await head.arrayBuffer()).byteLength, 0);
    assert.equal((await fetch(`${base}/../secret`)).status, 404);
    assert.equal((await fetch(`${base}/range.bin`, { method: 'POST' })).status, 405);
  } finally {
    fixture.server.closeAllConnections();
    await new Promise((resolve, reject) => fixture.server.close(error => error ? reject(error) : resolve()));
  }
});

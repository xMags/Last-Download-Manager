import assert from 'node:assert/strict';
import { test } from 'node:test';

import { FormPosts, rebuildBody, toBase64 } from '../src/form-posts.js';
import { makeCapture } from '../src/protocol.js';

const decode = (bytes) => new TextDecoder().decode(bytes);

test('url-encoded forms are rebuilt from their fields', () => {
  const body = rebuildBody({ formData: { token: ['abc'], tags: ['a', 'b c'] } }, 'application/x-www-form-urlencoded');
  assert.equal(body.contentType, 'application/x-www-form-urlencoded');
  assert.equal(decode(body.bytes), 'token=abc&tags=a&tags=b+c');
});

test('raw bodies are joined and keep their type', () => {
  const body = rebuildBody({ raw: [{ bytes: new TextEncoder().encode('{"a":').buffer }, { bytes: new TextEncoder().encode('1}').buffer }] }, 'application/json');
  assert.equal(body.contentType, 'application/json');
  assert.equal(decode(body.bytes), '{"a":1}');
});

test('forms with files cannot be rebuilt', () => {
  assert.equal(rebuildBody({ formData: { f: ['x.png'] } }, 'multipart/form-data; boundary=x'), null);
  assert.equal(rebuildBody({ raw: [{ file: 'C:/x.png' }] }, 'application/octet-stream'), null);
  assert.equal(rebuildBody({ error: 'Unknown error' }, ''), null);
  assert.equal(rebuildBody(undefined, ''), null);
});

test('a download is matched to a recent submission to its address', () => {
  let now = 0;
  const forms = new FormPosts(() => now);
  forms.onBeforeRequest({ requestId: '1', method: 'POST', url: 'https://a.test/get', requestBody: { formData: { token: ['abc'] } } });
  forms.onBeforeRequest({ requestId: '2', method: 'GET', url: 'https://a.test/other', requestBody: null });
  assert.equal(forms.postFor('https://a.test/get'), null, 'complete only once its headers are seen');
  forms.onSendHeaders({ requestId: '1', requestHeaders: [{ name: 'Content-Type', value: 'application/x-www-form-urlencoded' }] });
  assert.deepEqual(forms.postFor('https://a.test/get'), { contentType: 'application/x-www-form-urlencoded', body: toBase64(new TextEncoder().encode('token=abc')) });
  assert.equal(forms.postFor('https://a.test/other'), null);
  now += 31000;
  assert.equal(forms.postFor('https://a.test/get'), null, 'old submissions expire');
});

test('base64 matches the standard encoding', () => {
  assert.equal(toBase64(new TextEncoder().encode('token=abc')), 'dG9rZW49YWJj');
  assert.equal(toBase64(new Uint8Array([0, 255])), 'AP8=');
});

test('the capture message carries the form', () => {
  const message = makeCapture({ url: 'https://a.test/get', userAgent: 'UA', cookies: [], post: { contentType: 'application/x-www-form-urlencoded', body: 'dG9rZW49YWJj' } });
  assert.deepEqual(message.post, { contentType: 'application/x-www-form-urlencoded', body: 'dG9rZW49YWJj' });
  const unsafe = makeCapture({ url: 'https://a.test/get', userAgent: 'UA', cookies: [], post: { contentType: 'text/plain' + String.fromCharCode(13, 10) + 'X: 1', body: 'YQ==' } });
  assert.equal(unsafe.post, undefined);
});

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { DownloadCapture } from '../src/capture.js';
import { matchesAddressList, matchesExtensionList, matchesSiteList, nameFromUrl, shouldCapture } from '../src/capture-rules.js';
import { State } from '../src/connection.js';
import { isAcceptedCapture, makeCapture, toLdmCookie } from '../src/protocol.js';

const cases = JSON.parse(readFileSync(new URL('./capture-cases.json', import.meta.url), 'utf8'));

test('extension rules match the shared cases LDM is tested with', () => {
  for (const { patterns, name, capture } of cases) {
    assert.equal(matchesExtensionList(patterns, name), capture, `${name} with [${patterns}]`);
  }
});

test('only addresses LDM can download are captured', () => {
  const zip = { capturedExtensions: ['zip'] };
  assert.equal(shouldCapture({ url: 'https://a.test/x.zip', fileName: '' }, zip), true);
  assert.equal(shouldCapture({ url: 'blob:https://a.test/123', fileName: 'x.zip' }, zip), false);
  assert.equal(shouldCapture({ url: 'data:application/zip;base64,AA', fileName: 'x.zip' }, zip), false);
  assert.equal(shouldCapture({ url: 'https://a.test/get?id=1', fileName: 'x.zip' }, zip), true,
    'the browser-chosen name wins over the address');
});

test('site exceptions match hosts with wildcards, or host and path when they hold a slash', () => {
  assert.ok(matchesSiteList(['*.update.microsoft.com'], 'https://dl.update.microsoft.com/x.cab'));
  assert.ok(!matchesSiteList(['*.update.microsoft.com'], 'https://update.microsoft.com.evil.test/x.cab'));
  assert.ok(matchesSiteList(['files.test'], 'https://FILES.test/a.zip'));
  assert.ok(!matchesSiteList(['files.test'], 'https://cdn.files.test/a.zip'), 'a plain name is that host only');
  assert.ok(matchesSiteList(['files.test/private/*'], 'https://files.test/private/a.zip'));
  assert.ok(!matchesSiteList(['files.test/private/*'], 'https://files.test/public/a.zip'));
  assert.ok(!matchesSiteList(['x.test'], 'not a url'));
});

test('address exceptions match the whole address', () => {
  assert.ok(matchesAddressList(['https://a.test/get?id=*'], 'https://a.test/get?id=7'));
  assert.ok(!matchesAddressList(['https://a.test/get?id=*'], 'https://a.test/other'));
  assert.ok(matchesAddressList(['*.iso'], 'https://mirror.test/linux.ISO'));
});

test('an exception on the link or on where it led keeps the download in the browser', () => {
  const rules = { capturedExtensions: ['zip'], siteExceptions: ['cdn.test'], addressExceptions: ['https://a.test/keep/*'] };
  assert.equal(shouldCapture({ url: 'https://a.test/x.zip', fileName: 'x.zip' }, rules), true);
  assert.equal(shouldCapture({ url: 'https://a.test/x.zip', finalUrl: 'https://cdn.test/x.zip', fileName: 'x.zip' }, rules), false);
  assert.equal(shouldCapture({ url: 'https://a.test/keep/x.zip', fileName: 'x.zip' }, rules), false);
});

test('names come from the address path, decoded', () => {
  assert.equal(nameFromUrl('https://a.test/dir/My%20File.zip?x=1'), 'My File.zip');
  assert.equal(nameFromUrl('https://a.test/'), '');
  assert.equal(nameFromUrl('not a url'), '');
});

test('browser cookies are converted and unsafe ones dropped', () => {
  assert.deepEqual(
    toLdmCookie({ domain: '.a.test', hostOnly: false, path: '/', secure: true, session: false, expirationDate: 1900000000.7, name: 'n', value: 'v' }),
    { domain: 'a.test', hostOnly: false, path: '/', secure: true, expires: 1900000000, name: 'n', value: 'v' });
  assert.equal(toLdmCookie({ domain: 'a.test', hostOnly: true, path: '/', session: true, name: 'n', value: 'a\nb' }), null);
  assert.equal(toLdmCookie({ domain: 'a test', hostOnly: true, path: '/', session: true, name: 'n', value: 'v' }), null);
  assert.equal(toLdmCookie({ domain: 'a.test', hostOnly: true, path: '/', session: true, name: '', value: 'v' }), null);
});

test('capture messages drop duplicate cookies and empty fields', () => {
  const cookie = { domain: 'a.test', hostOnly: true, path: '/', session: true, name: 'sid', value: '1' };
  const message = makeCapture({
    url: 'https://a.test/x.zip', finalUrl: 'https://a.test/x.zip', referrer: 'chrome://newtab', fileName: 'x.zip',
    fileSize: -1, mimeType: '', userAgent: 'UA', cookies: [cookie, cookie],
  });
  assert.equal(message.type, 'capture');
  assert.equal(message.cookies.length, 1);
  assert.equal(message.finalUrl, undefined, 'same as url');
  assert.equal(message.referrer, undefined, 'not an address LDM can use');
  assert.equal(message.fileSize, undefined);
  assert.equal(message.mimeType, undefined);
  assert.ok(isAcceptedCapture({ v: 1, type: 'capture-result', id: message.id, accepted: true }, message.id));
  assert.ok(!isAcceptedCapture({ v: 1, type: 'capture-result', id: 'other', accepted: true }, message.id));
  assert.ok(!isAcceptedCapture({ v: 1, type: 'capture-result', id: message.id, accepted: false }, message.id));
});

// A browser downloads API that records what the capture did.
function fakeBrowser({ stateAfterPause = 'in_progress' } = {}) {
  const calls = [];
  const api = {
    runtime: { id: 'self' },
    downloads: {
      pause: async (id) => { calls.push(['pause', id]); },
      resume: async (id) => { calls.push(['resume', id]); },
      cancel: async (id) => { calls.push(['cancel', id]); },
      erase: async (query) => { calls.push(['erase', query.id]); },
      search: async ({ id }) => [{ id, state: stateAfterPause }],
      setUiOptions: async ({ enabled }) => { calls.push(['ui', enabled]); },
    },
    cookies: { getAll: async ({ url }) => [{ domain: new URL(url).hostname, hostOnly: true, path: '/', session: true, name: 'sid', value: '1' }] },
  };
  return { api, calls };
}

// Each capture goes on its own connection (oneShot); 'none' is LDM unreachable or silent.
function fakeConnection({ answer = 'accept' } = {}) {
  const sent = [];
  return {
    sent,
    state: State.Connected,
    oneShot: async (message) => {
      sent.push(['oneShot', message]);
      if (answer === 'none') {
        return null;
      }
      return { v: 1, type: 'capture-result', id: message.id, accepted: answer === 'accept' };
    },
  };
}

function newCapture(api, connection, { enabled = true } = {}) {
  return new DownloadCapture(api, connection, {
    ready: Promise.resolve(),
    isEnabled: () => enabled,
    rules: () => ({ capturedExtensions: ['zip'], siteExceptions: [], addressExceptions: [] }),
    userAgent: 'TestUA',
    canHideDownloadUi: true,
  });
}

const ITEM = { id: 7, state: 'in_progress', url: 'https://a.test/x.zip', finalUrl: 'https://cdn.test/x.zip', referrer: 'https://a.test/page', totalBytes: 100, mime: 'application/zip', incognito: false };

test('an accepted capture cancels and erases the browser download', async () => {
  const { api, calls } = fakeBrowser();
  const connection = fakeConnection();
  assert.equal(await newCapture(api, connection).take(ITEM, 'x.zip'), true);
  assert.deepEqual(calls, [['ui', false], ['pause', 7], ['cancel', 7], ['erase', 7], ['ui', true]]);
  assert.equal(connection.sent.length, 1);
  const message = connection.sent[0][1];
  assert.equal(message.url, 'https://a.test/x.zip');
  assert.equal(message.finalUrl, 'https://cdn.test/x.zip');
  assert.equal(message.userAgent, 'TestUA');
  assert.deepEqual(message.cookies.map((c) => c.domain).sort(), ['a.test', 'cdn.test'], 'cookies of both addresses');
});

test('a refused or unanswered capture lets the browser continue', async () => {
  for (const answer of ['refuse', 'none']) {
    const { api, calls } = fakeBrowser();
    assert.equal(await newCapture(api, fakeConnection({ answer })).take(ITEM, 'x.zip'), false);
    assert.deepEqual(calls, [['ui', false], ['pause', 7], ['resume', 7], ['ui', true]], answer);
  }
});

test('a download that already finished is left alone', async () => {
  const { api, calls } = fakeBrowser({ stateAfterPause: 'complete' });
  const connection = fakeConnection();
  assert.equal(await newCapture(api, connection).take(ITEM, 'x.zip'), false);
  assert.deepEqual(calls, [['ui', false], ['pause', 7], ['ui', true]]);
  assert.equal(connection.sent.length, 0);
});

test('nothing happens when integration is off, the type is not captured, or the window is private', async () => {
  for (const [item, name, enabled] of [[ITEM, 'x.zip', false], [ITEM, 'x.txt', true], [{ ...ITEM, incognito: true }, 'x.zip', true]]) {
    const { api, calls } = fakeBrowser();
    const connection = fakeConnection();
    assert.equal(await newCapture(api, connection, { enabled }).take(item, name), false);
    assert.deepEqual(calls, []);
    assert.equal(connection.sent.length, 0);
  }
});

test('the download bubble stays hidden until the last capture ends', async () => {
  const { api, calls } = fakeBrowser();
  const capture = newCapture(api, fakeConnection());
  await Promise.all([capture.take({ ...ITEM, id: 1 }, 'a.zip'), capture.take({ ...ITEM, id: 2 }, 'b.zip')]);
  assert.deepEqual(calls.filter(([kind]) => kind === 'ui'), [['ui', false], ['ui', true]]);
});

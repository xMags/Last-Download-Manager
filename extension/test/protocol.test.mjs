import assert from 'node:assert/strict';
import { test } from 'node:test';

import { cleanText, makeHello, parseConfig } from '../src/protocol.js';

test('hello carries every field LDM requires', () => {
  const hello = makeHello({
    browser: 'Chrome', browserVersion: '141', extensionVersion: '5.0.0', incognitoAllowed: true, startLdm: false,
  });
  assert.equal(hello.v, 1);
  assert.equal(hello.type, 'hello');
  assert.match(hello.id, /^m\d+$/);
  assert.equal(hello.browser, 'Chrome');
  assert.equal(hello.incognitoAllowed, true);
  assert.equal(hello.startLdm, false);
});

test('hello ids are unique', () => {
  const first = makeHello({ browser: 'x', extensionVersion: '1' });
  const second = makeHello({ browser: 'x', extensionVersion: '1' });
  assert.notEqual(first.id, second.id);
});

test('hello text is cut to what LDM accepts', () => {
  const hello = makeHello({ browser: `Edge\n${'x'.repeat(100)}`, browserVersion: 'é1', extensionVersion: '' });
  assert.equal(hello.browser.length, 64);
  assert.ok(!hello.browser.includes('\n'));
  assert.equal(hello.browserVersion, '1');
  assert.equal(hello.extensionVersion, '0');
  assert.equal(hello.startLdm, false);
});

test('cleanText removes control and non-ASCII characters', () => {
  assert.equal(cleanText('a\u0000béc', 10), 'abc');
  assert.equal(cleanText(undefined, 10), '');
});

test('config is parsed and bad patterns are dropped', () => {
  const config = parseConfig({
    v: 1, id: 'push', type: 'config', appVersion: '5.0.0',
    capturedExtensions: ['zip', 'r0*', 'BAD', 'a b', 7, '', 'x'.repeat(33)],
  });
  assert.deepEqual(config, { appVersion: '5.0.0', capturedExtensions: ['zip', 'r0*'], siteExceptions: [], addressExceptions: [],
    keys: { prevent: { enabled: true, alt: true, shift: false, ctrl: false }, force: { enabled: true, alt: false, shift: false, ctrl: false, insert: true, delete: false }, forceNeedsLinkClick: true, skipWebContent: true },
    menu: { downloadLink: true, downloadAll: true },
    media: { enabled: true, types: [], minimumBytes: 0, siteExceptions: [] } });
});

test('anything that is not a config is refused', () => {
  assert.equal(parseConfig(null), null);
  assert.equal(parseConfig('config'), null);
  assert.equal(parseConfig({ v: 2, type: 'config', appVersion: '1', capturedExtensions: [] }), null);
  assert.equal(parseConfig({ v: 1, type: 'hello', appVersion: '1', capturedExtensions: [] }), null);
  assert.equal(parseConfig({ v: 1, type: 'config', appVersion: '1', capturedExtensions: 'zip' }), null);
});

test('exception lists keep only patterns without spaces or control characters', () => {
  const config = parseConfig({
    v: 1, id: 'push', type: 'config', appVersion: '5.0.0', capturedExtensions: [],
    siteExceptions: ['*.a.test', 'b.test/x/*', 'bad site', 7, ''],
    addressExceptions: ['https://a.test/*', 'x' + String.fromCharCode(10) + 'y'],
  });
  assert.deepEqual(config.siteExceptions, ['*.a.test', 'b.test/x/*']);
  assert.deepEqual(config.addressExceptions, ['https://a.test/*']);
});

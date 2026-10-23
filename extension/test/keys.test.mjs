import assert from 'node:assert/strict';
import { test } from 'node:test';

import { downloadVerdict, isWebContent, keySettings, keyVerdict, NO_KEYS } from '../src/key-rules.js';
import { cleanKeys, LinkClicks } from '../src/link-clicks.js';

const defaults = keySettings({});
const keys = (pressed) => ({ ...NO_KEYS, ...pressed });

test('missing or malformed settings fall back to LDM defaults', () => {
  assert.deepEqual(keySettings({ keys: { prevent: { alt: 'yes' }, force: null } }), defaults);
  assert.equal(defaults.prevent.alt, true);
  assert.equal(defaults.force.insert, true);
});

test('a click with the prevent key keeps the download in the browser', () => {
  assert.equal(keyVerdict(defaults, keys({ alt: true }), NO_KEYS), 'prevent');
  assert.equal(keyVerdict(defaults, null, keys({ alt: true })), 'prevent', 'held while the download starts');
  assert.equal(keyVerdict(defaults, keys({ shift: true }), NO_KEYS), 'none');
});

test('every key of a set must be held', () => {
  const settings = keySettings({ keys: { prevent: { enabled: true, alt: true, shift: true, ctrl: false } } });
  assert.equal(keyVerdict(settings, keys({ alt: true }), NO_KEYS), 'none');
  assert.equal(keyVerdict(settings, keys({ alt: true, shift: true }), NO_KEYS), 'prevent');
});

test('a turned-off set or a set without keys never acts', () => {
  const off = keySettings({ keys: { prevent: { enabled: false, alt: true }, force: { enabled: true, insert: false } } });
  assert.equal(keyVerdict(off, keys({ alt: true, insert: true }), NO_KEYS), 'none');
});

test('forcing needs the click unless the settings allow held keys', () => {
  assert.equal(keyVerdict(defaults, keys({ insert: true }), NO_KEYS), 'force');
  assert.equal(keyVerdict(defaults, null, keys({ insert: true })), 'none');
  const delayed = keySettings({ keys: { forceNeedsLinkClick: false } });
  assert.equal(keyVerdict(delayed, null, keys({ insert: true })), 'force');
});

test('prevent wins over force', () => {
  const both = keySettings({ keys: { prevent: { enabled: true, alt: true }, force: { enabled: true, alt: true, insert: false } } });
  assert.equal(keyVerdict(both, keys({ alt: true }), NO_KEYS), 'prevent');
});

test('forcing leaves web pages, pictures and scripts to the browser when asked', () => {
  assert.equal(downloadVerdict(defaults, keys({ insert: true }), NO_KEYS, 'text/html', 'page'), 'none');
  assert.equal(downloadVerdict(defaults, keys({ insert: true }), NO_KEYS, '', 'photo.JPG'), 'none');
  assert.equal(downloadVerdict(defaults, keys({ insert: true }), NO_KEYS, 'application/octet-stream', 'tool.bin'), 'force');
  const all = keySettings({ keys: { skipWebContent: false } });
  assert.equal(downloadVerdict(all, keys({ insert: true }), NO_KEYS, 'text/html', 'page.html'), 'force');
  assert.ok(isWebContent('image/png', ''));
  assert.ok(!isWebContent('application/zip', 'a.zip'));
});

test('a download is matched to the click on its link, or to a fresh click on its page', () => {
  let now = 1000;
  const clicks = new LinkClicks(() => now);
  clicks.recordClick({ href: 'https://a.test/x.zip', pageUrl: 'https://a.test/', keys: keys({ insert: true }) });
  assert.deepEqual(clicks.clickKeysFor({ url: 'https://a.test/x.zip' }), keys({ insert: true }));
  assert.deepEqual(clicks.clickKeysFor({ url: 'https://cdn.test/y', finalUrl: 'https://a.test/x.zip' }), keys({ insert: true }));
  assert.deepEqual(clicks.clickKeysFor({ url: 'https://b.test/z', referrer: 'https://a.test/' }), keys({ insert: true }));
  now += 6000;
  assert.equal(clicks.clickKeysFor({ url: 'https://b.test/z', referrer: 'https://a.test/' }), null, 'page match is short-lived');
  assert.ok(clicks.clickKeysFor({ url: 'https://a.test/x.zip' }));
  now += 10000;
  assert.equal(clicks.clickKeysFor({ url: 'https://a.test/x.zip' }), null, 'old clicks expire');
});

test('held keys expire when a page stops reporting', () => {
  let now = 0;
  const clicks = new LinkClicks(() => now);
  clicks.recordKeys(keys({ alt: true }));
  assert.equal(clicks.heldKeys().alt, true);
  now += 61000;
  assert.deepEqual(clicks.heldKeys(), NO_KEYS);
});

test('key reports from pages keep only five booleans', () => {
  assert.deepEqual(cleanKeys({ alt: 'true', shift: true, extra: true }), keys({ shift: true }));
  assert.deepEqual(cleanKeys(null), NO_KEYS);
});

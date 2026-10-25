import assert from 'node:assert/strict';
import { test } from 'node:test';

import { OffTabDownloads } from '../src/off-tab-downloads.js';

function setup({ tabs = [], off = [] } = {}) {
  const offTabs = new Set(off);
  let time = 1000;
  const api = { tabs: { query: async () => tabs } };
  const downloads = new OffTabDownloads(api, (tabId) => offTabs.has(tabId), () => time);
  return { downloads, offTabs, advance: (ms) => { time += ms; } };
}

test('a link that loaded in a switched-off tab and became a download is left to the browser', async () => {
  const { downloads, offTabs } = setup({ off: [4] });
  downloads.onBeforeRequest({ tabId: 4, url: 'https://a.test/go?id=1', type: 'main_frame' });
  downloads.onBeforeRequest({ tabId: 4, url: 'https://cdn.test/file.zip', type: 'main_frame' });  // the redirect
  assert.equal(await downloads.fromOffTab({ url: 'https://a.test/go?id=1', finalUrl: 'https://cdn.test/file.zip' }), true);
  offTabs.delete(4);
  assert.equal(await downloads.fromOffTab({ url: 'https://a.test/go?id=1' }), false, 'turned on again since');
});

test('requests of switched-on tabs and of no tab are not kept, and kept ones expire', async () => {
  const { downloads, advance } = setup({ off: [4] });
  downloads.onBeforeRequest({ tabId: 5, url: 'https://a.test/on.zip' });
  downloads.onBeforeRequest({ tabId: -1, url: 'https://a.test/none.zip' });
  assert.equal(downloads.requests.length, 0);
  downloads.onBeforeRequest({ tabId: 4, url: 'https://a.test/old.zip' });
  advance(61000);
  assert.equal(await downloads.fromOffTab({ url: 'https://a.test/old.zip' }), false);
});

test('the referrer counts only when every tab showing that page is switched off', async () => {
  const page = 'https://a.test/page';
  const offOnly = setup({ tabs: [{ id: 4, url: page }, { id: 6, url: 'https://b.test/' }], off: [4] });
  assert.equal(await offOnly.downloads.fromOffTab({ url: 'blob:https://a.test/1', referrer: page }), true);
  const both = setup({ tabs: [{ id: 4, url: page }, { id: 5, url: page }], off: [4] });
  assert.equal(await both.downloads.fromOffTab({ url: 'https://a.test/x.zip', referrer: page }), false,
    'the same page in a switched-on tab keeps its downloads');
  assert.equal(await offOnly.downloads.fromOffTab({ url: 'https://a.test/x.zip', referrer: 'https://c.test/p' }), false);
  assert.equal(await offOnly.downloads.fromOffTab({ url: 'https://a.test/x.zip' }), false, 'no referrer, no sign');
});

test('a referrer that names only the site matches the tabs of that site', async () => {
  const tabs = [{ id: 4, url: 'https://a.test/watch?v=1' }, { id: 6, url: 'https://b.test/' }];
  const { downloads } = setup({ tabs, off: [4] });
  assert.equal(await downloads.fromOffTab({ url: 'https://cdn.test/x.zip', referrer: 'https://a.test/' }), true);
  const mixed = setup({ tabs: [...tabs, { id: 7, url: 'https://a.test/other' }], off: [4] });
  assert.equal(await mixed.downloads.fromOffTab({ url: 'https://cdn.test/x.zip', referrer: 'https://a.test/' }), false);
});

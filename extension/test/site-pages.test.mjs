import assert from 'node:assert/strict';
import { test } from 'node:test';

import { makeSitePage, parseConfig } from '../src/protocol.js';
import { isSiteAddress, SitePages } from '../src/site-pages.js';

function fakeApi(tabs = []) {
  return { tabs: { query: async () => tabs } };
}

function recorder() {
  const posted = [];
  return { posted, post: (message) => { posted.push(message); return true; } };
}

function pages(connection, { enabled = true, tabs = [] } = {}) {
  return new SitePages(fakeApi(tabs), connection, { ready: Promise.resolve(), isEnabled: () => enabled, userAgent: 'UA/1' });
}

const watch = (id) => `https://www.youtube.com/watch?v=${id}`;

test('only the supported sites own addresses count', () => {
  assert.equal(isSiteAddress(watch('aqz-KE-bpKQ')), true);
  assert.equal(isSiteAddress('https://youtu.be/aqz-KE-bpKQ'), true);
  assert.equal(isSiteAddress('https://m.youtube.com/'), true);
  assert.equal(isSiteAddress('https://www.youtube-nocookie.com/embed/aqz-KE-bpKQ'), true);
  assert.equal(isSiteAddress('https://youtube.com.example.test/watch?v=1'), false);
  assert.equal(isSiteAddress('https://example.test/?u=https://www.youtube.com/'), false);
  assert.equal(isSiteAddress('ftp://www.youtube.com/'), false);
  assert.equal(isSiteAddress(undefined), false);
});

test('a site address is sent once per change, other addresses never', async () => {
  const connection = recorder();
  const sitePages = pages(connection);
  await sitePages.onUpdated(3, { url: watch('aqz-KE-bpKQ') }, { url: watch('aqz-KE-bpKQ'), title: 'Bunny - YouTube' });
  await sitePages.onUpdated(3, { status: 'complete' }, { url: watch('aqz-KE-bpKQ'), title: 'Bunny - YouTube' });
  await sitePages.onUpdated(3, { title: 'New title' }, { url: watch('jNQXAC9IVRw') });  // neither address nor load
  await sitePages.onUpdated(4, { url: 'https://example.test/' }, { url: 'https://example.test/' });
  assert.equal(connection.posted.length, 1);
  const [message] = connection.posted;
  assert.equal(message.type, 'site-page');
  assert.equal(message.tabId, 3);
  assert.equal(message.url, watch('aqz-KE-bpKQ'));
  assert.equal(message.pageTitle, 'Bunny - YouTube');
  assert.equal(message.userAgent, 'UA/1');
  assert.equal(message.incognito, false);

  // The site changes the video without loading a page.
  await sitePages.onUpdated(3, { url: watch('jNQXAC9IVRw') }, { url: watch('jNQXAC9IVRw') });
  // And leaves it for another page of the site, which ends LDM's offer.
  await sitePages.onUpdated(3, { url: 'https://www.youtube.com/feed/subscriptions' }, { url: 'https://www.youtube.com/feed/subscriptions' });
  assert.deepEqual(connection.posted.map((m) => m.url), [watch('aqz-KE-bpKQ'), watch('jNQXAC9IVRw'),
    'https://www.youtube.com/feed/subscriptions']);
});

test('a page loaded again at the same address is sent again', async () => {
  const connection = recorder();
  const sitePages = pages(connection);
  const tab = { url: watch('aqz-KE-bpKQ') };
  await sitePages.onUpdated(3, { url: tab.url }, tab);
  sitePages.reset(3);  // the reload's main frame request
  await sitePages.onUpdated(3, { status: 'complete' }, tab);
  assert.equal(connection.posted.length, 2);
});

test('nothing is sent unless LDM asked for site pages', async () => {
  const connection = recorder();
  const sitePages = pages(connection, { enabled: false });
  await sitePages.onUpdated(3, { url: watch('aqz-KE-bpKQ') }, { url: watch('aqz-KE-bpKQ') });
  await sitePages.announceAll();
  assert.equal(connection.posted.length, 0);
  assert.equal(parseConfig({ v: 1, type: 'config', appVersion: '5.0.0', capturedExtensions: [] }).sitePages, false,
    'an LDM that says nothing gets none');
  assert.equal(parseConfig({ v: 1, type: 'config', appVersion: '5.1.0', capturedExtensions: [], sitePages: true }).sitePages,
    true);
});

test('announcing sends every open site tab, again after a new connection', async () => {
  const connection = recorder();
  const tabs = [{ id: 1, url: watch('aqz-KE-bpKQ') }, { id: 2, url: 'https://example.test/' },
    { id: 3, url: 'https://youtu.be/jNQXAC9IVRw', incognito: true }];
  const sitePages = pages(connection, { tabs });
  await sitePages.announceAll();
  await sitePages.announceAll();
  assert.deepEqual(connection.posted.map((m) => m.tabId), [1, 3, 1, 3]);
  assert.equal(connection.posted[1].incognito, true);
});

test('the message carries only what LDM accepts', () => {
  assert.equal(makeSitePage({ tabId: -1, url: watch('a'), userAgent: 'x' }), null);
  assert.equal(makeSitePage({ tabId: 1, url: 'javascript:void(0)', userAgent: 'x' }), null);
  const message = makeSitePage({ tabId: 1, url: watch('a'), pageTitle: 'T\nitle', userAgent: '' });
  assert.equal(message.userAgent, 'Mozilla/5.0');
  assert.ok(!/[\n]/.test(message.pageTitle ?? ''));
});

test('the sites LDM lists are matched with their subdomains, and only valid host names are taken', () => {
  const config = parseConfig({
    v: 1, id: 'push', type: 'config', appVersion: '5.2.0', capturedExtensions: [], sitePages: true,
    siteHosts: ['youtube.com', 'dailymotion.com', 'bandcamp.com', 'Bad.Host', 'nodot', '*.example.test', 7, 'a b.test'],
  });
  assert.deepEqual(config.siteHosts, ['youtube.com', 'dailymotion.com', 'bandcamp.com']);
  assert.equal(isSiteAddress('https://www.dailymotion.com/video/x5kesuj', config.siteHosts), true);
  assert.equal(isSiteAddress('https://artist.bandcamp.com/track/song', config.siteHosts), true);
  assert.equal(isSiteAddress('https://notdailymotion.com/video/x', config.siteHosts), false);
  assert.equal(isSiteAddress('https://vimeo.com/76979871', config.siteHosts), false);
  assert.equal(parseConfig({ v: 1, type: 'config', appVersion: '5.1.0', capturedExtensions: [] }).siteHosts, null);
});

test('without a list from LDM, YouTube is the site', async () => {
  const connection = recorder();
  const sitePages = new SitePages(fakeApi(), connection,
    { ready: Promise.resolve(), isEnabled: () => true, hosts: () => null, userAgent: 'UA/1' });
  await sitePages.onUpdated(1, { url: 'https://www.dailymotion.com/video/x5kesuj' }, { url: 'https://www.dailymotion.com/video/x5kesuj' });
  await sitePages.onUpdated(2, { url: watch('aqz-KE-bpKQ') }, { url: watch('aqz-KE-bpKQ') });
  assert.deepEqual(connection.posted.map((m) => m.tabId), [2]);

  const listed = recorder();
  const withList = new SitePages(fakeApi(), listed,
    { ready: Promise.resolve(), isEnabled: () => true, hosts: () => ['dailymotion.com'], userAgent: 'UA/1' });
  await withList.onUpdated(1, { url: 'https://www.dailymotion.com/video/x5kesuj' }, { url: 'https://www.dailymotion.com/video/x5kesuj' });
  await withList.onUpdated(2, { url: watch('aqz-KE-bpKQ') }, { url: watch('aqz-KE-bpKQ') });
  assert.deepEqual(listed.posted.map((m) => m.tabId), [1]);
});

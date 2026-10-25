import assert from 'node:assert/strict';
import { test } from 'node:test';

import { clickedAddress, clickedVideoPage, menuItems, MenuId } from '../src/context-menu.js';
import { makeSiteRequest } from '../src/protocol.js';
import { detailsFromHeaders, fileNameFromDisposition, LinkChecker } from '../src/link-check.js';
import {
  cleanPageText, isLinksDone, linksAccepted, makeLinkInfo, makeLinks, MAX_PAGE_LINKS, parseConfig, utf8Length,
} from '../src/protocol.js';

const COOKIE = { domain: '.a.test', hostOnly: false, path: '/', secure: true, session: true, name: 'sid', value: 'x' };

function page(links, cookies = []) {
  return { selection: false, referrer: 'https://a.test/page', userAgent: 'Mozilla/5.0', links, cookies };
}

test('links keep what LDM accepts and drop the rest', () => {
  const message = makeLinks(page([
    { url: 'https://a.test/a.zip', text: '  Setup\n\tfile ', kind: 'link', fileName: 'setup.zip' },
    { url: 'javascript:alert(1)', text: 'x', kind: 'link' },
    { url: 'https://a.test/p.jpg', text: '', kind: 'image' },
    { url: 'https://a.test/x', kind: 'script' },
    { url: 'ftp://a.test/b.iso', kind: 'text' },
  ], [COOKIE, COOKIE, { ...COOKIE, name: '' }]));
  assert.equal(message.type, 'links');
  assert.equal(message.selection, false);
  assert.equal(message.referrer, 'https://a.test/page');
  assert.deepEqual(message.links, [
    { url: 'https://a.test/a.zip', kind: 'link', text: 'Setup file', fileName: 'setup.zip' },
    { url: 'https://a.test/p.jpg', kind: 'image' },
    { url: 'ftp://a.test/b.iso', kind: 'text' },
  ]);
  assert.equal(message.cookies.length, 1, 'duplicates and invalid cookies are dropped');
  assert.equal(message.cookies[0].domain, 'a.test');
});

test('a page without usable links gives no message', () => {
  assert.equal(makeLinks(page([{ url: 'data:text/plain,x', kind: 'link' }])), null);
});

test('the links message stays within LDM limits', () => {
  const many = Array.from({ length: MAX_PAGE_LINKS + 10 }, (_, i) => ({ url: `https://a.test/${i}.zip`, kind: 'link' }));
  assert.equal(makeLinks(page(many)).links.length, MAX_PAGE_LINKS);

  const long = Array.from({ length: 4000 }, (_, i) => ({
    url: `https://a.test/${i}/${'p'.repeat(2000)}.zip`, text: 'é'.repeat(600), kind: 'link',
  }));
  const message = makeLinks(page(long));
  assert.ok(message.links.length > 0 && message.links.length < long.length);
  assert.ok(utf8Length(JSON.stringify(message)) < 4 * 1024 * 1024);
});

test('page text is made valid for LDM', () => {
  assert.equal(cleanPageText('a\u0000b\u007f c\r\n', 100), 'a b c');
  assert.equal(cleanPageText('x\uD800y', 100), 'x�y', 'a lone surrogate cannot be sent as UTF-8');
  assert.equal(cleanPageText('😀😀', 5), '😀', 'cut between characters, not inside one');
  assert.equal(utf8Length(cleanPageText('é'.repeat(2000), 1024)), 1024);
  assert.equal(cleanPageText(undefined, 10), '');
});

test('answers to the links message are recognised by id', () => {
  assert.equal(linksAccepted({ v: 1, id: 'm1', type: 'links-result', accepted: true }, 'm1'), true);
  assert.equal(linksAccepted({ v: 1, id: 'm1', type: 'links-result', accepted: false }, 'm1'), false);
  assert.equal(linksAccepted({ v: 1, id: 'm2', type: 'links-result', accepted: true }, 'm1'), false);
  assert.equal(isLinksDone({ v: 1, id: 'm1', type: 'links-done' }, 'm1'), true);
  assert.equal(isLinksDone({ v: 1, id: 'm1', type: 'links-result' }, 'm1'), false);
});

test('link details are cleaned before they go to LDM', () => {
  const message = makeLinkInfo([
    { index: 0, fileSize: 5, mimeType: 'application/zip', fileName: 'a.zip' },
    { index: 1, failed: true, fileSize: 3 },
    { index: 2, fileSize: -1, mimeType: 'text/html\r\nX: y', fileName: '' },
    { index: -1 },
    { index: 1.5 },
  ]);
  assert.equal(message.type, 'link-info');
  assert.deepEqual(JSON.parse(JSON.stringify(message.items)), [
    { index: 0, fileSize: 5, mimeType: 'application/zip', fileName: 'a.zip' },
    { index: 1, failed: true },
    { index: 2, mimeType: 'text/html X: y' },
  ]);
});

test('the config says which menu items appear, on by default', () => {
  const base = { v: 1, id: 'm1', type: 'config', appVersion: '5.0.0', capturedExtensions: [] };
  assert.deepEqual(parseConfig(base).menu, { downloadLink: true, downloadAll: true });
  assert.deepEqual(parseConfig({ ...base, menu: { downloadLink: false, downloadAll: 'no' } }).menu,
    { downloadLink: false, downloadAll: true });
});

// The page items, without the toolbar button's own switch.
const pageItems = (menu) => menuItems(menu).filter((item) => item.id !== MenuId.Integration);

test('menu items follow the settings', () => {
  const ids = (menu) => pageItems(menu).map((item) => item.id);
  assert.deepEqual(ids({ downloadLink: true, downloadAll: true }), [MenuId.Link, MenuId.Page, MenuId.Selected, MenuId.All]);
  assert.deepEqual(ids({ downloadLink: false, downloadAll: true }), [MenuId.All]);
  assert.deepEqual(ids({ downloadLink: true, downloadAll: false }), [MenuId.Link, MenuId.Page, MenuId.Selected]);
  const all = pageItems({ downloadLink: true, downloadAll: true });
  assert.ok(all.every((item) => item.title.endsWith('with LDM')));
});

test('the video items appear only when LDM takes site requests', () => {
  const ids = (menu) => pageItems(menu).map((item) => item.id);
  assert.deepEqual(ids({ downloadLink: false, downloadAll: false, videos: true }), [MenuId.VideoPage, MenuId.VideoLink]);
  assert.deepEqual(ids({ downloadLink: false, downloadAll: false, videos: false }), []);
  const [page, link] = pageItems({ downloadLink: false, downloadAll: false, videos: true });
  assert.deepEqual(page.contexts, ['page', 'frame', 'video']);
  assert.deepEqual(link.contexts, ['link']);
  assert.equal(clickedVideoPage({ menuItemId: MenuId.VideoLink, linkUrl: 'https://v.test/1', pageUrl: 'https://a.test/' }),
    'https://v.test/1');
  assert.equal(clickedVideoPage({ menuItemId: MenuId.VideoPage, pageUrl: 'https://a.test/', frameUrl: 'https://p.test/e' }),
    'https://p.test/e');
});

test('the toolbar button always has the integration switch, checked while integration is on', () => {
  const switchOf = (menu) => menuItems(menu).find((item) => item.id === MenuId.Integration);
  const on = switchOf({ downloadLink: false, downloadAll: false });
  assert.deepEqual(on.contexts, ['action']);
  assert.equal(on.type, 'checkbox');
  assert.equal(on.checked, true);
  assert.equal(switchOf({ downloadLink: true, downloadAll: true, enabled: false }).checked, false);
});

test('a site request carries only web addresses', () => {
  const request = makeSiteRequest({ url: 'https://v.test/1', referrer: 'https://a.test/', pageTitle: 'T', userAgent: 'UA/1' });
  assert.equal(request.type, 'site-request');
  assert.equal(request.url, 'https://v.test/1');
  assert.equal(request.referrer, 'https://a.test/');
  assert.equal(request.incognito, false);
  assert.equal(makeSiteRequest({ url: 'https://v.test/1', referrer: 'blob:https://a.test/x' }).referrer, undefined);
  assert.equal(makeSiteRequest({ url: 'file:///C:/x' }), null);
  assert.equal(makeSiteRequest({ url: undefined }), null);
});

test('a menu click is about the link, else the picture, else the page', () => {
  assert.equal(clickedAddress({ menuItemId: MenuId.Link, linkUrl: 'https://a.test/l', srcUrl: 'https://a.test/i.jpg' }),
    'https://a.test/l');
  assert.equal(clickedAddress({ menuItemId: MenuId.Link, srcUrl: 'https://a.test/v.mp4' }), 'https://a.test/v.mp4');
  assert.equal(clickedAddress({ menuItemId: MenuId.Page, pageUrl: 'https://a.test/', frameUrl: 'https://b.test/f' }),
    'https://b.test/f');
});

test('file names come from Content-Disposition', () => {
  assert.equal(fileNameFromDisposition('attachment; filename="a b.zip"'), 'a b.zip');
  assert.equal(fileNameFromDisposition('attachment; filename=plain.iso'), 'plain.iso');
  assert.equal(fileNameFromDisposition(`attachment; filename="x.zip"; filename*=UTF-8''caf%C3%A9.zip`), 'café.zip');
  assert.equal(fileNameFromDisposition(`attachment; filename*=iso-8859-1''caf%E9.zip`), 'café.zip');
  assert.equal(fileNameFromDisposition('attachment; filename="q\\"uote.txt"'), 'q"uote.txt');
  assert.equal(fileNameFromDisposition('inline'), undefined);
  assert.equal(fileNameFromDisposition(`attachment; filename*=UTF-8''%E0%A4.zip; filename="fallback.zip"`), 'fallback.zip');
});

test('details are read from the headers', () => {
  const headers = new Headers({ 'content-length': '1234', 'content-type': 'application/zip' });
  assert.deepEqual(detailsFromHeaders(headers), { fileSize: 1234, mimeType: 'application/zip', fileName: undefined });
  assert.equal(detailsFromHeaders(new Headers({ 'content-length': 'abc' })).fileSize, undefined);
});

function response(status, headers = {}) {
  return { status, ok: status >= 200 && status < 300, headers: new Headers(headers) };
}

test('links are checked once per address, with GET where HEAD is refused', async () => {
  const requests = [];
  const fetchFn = async (url, init) => {
    requests.push(`${init.method} ${url}`);
    assert.equal(init.credentials, 'include');
    if (url.endsWith('/nohead.zip') && init.method === 'HEAD') {
      return response(405);
    }
    if (url.endsWith('/gone.zip')) {
      return response(404);
    }
    if (url.endsWith('/down.zip')) {
      throw new TypeError('network');
    }
    return response(200, { 'content-length': '10', 'content-type': 'application/zip' });
  };
  const batches = [];
  const checker = new LinkChecker(fetchFn, (items) => batches.push(items));
  await checker.run([
    'https://a.test/a.zip', 'https://a.test/a.zip#part', 'https://a.test/nohead.zip', 'https://a.test/gone.zip',
    'https://a.test/down.zip', 'ftp://a.test/f.iso',
  ]);
  const items = batches.flat().sort((a, b) => a.index - b.index);
  assert.deepEqual(items.map((item) => item.index), [0, 1, 2, 3, 4, 5]);
  assert.equal(items[0].fileSize, 10);
  assert.equal(items[1].fileSize, 10, 'the same address with a #fragment shares the answer');
  assert.equal(items[2].mimeType, 'application/zip');
  assert.equal(items[3].failed, true);
  assert.equal(items[4].failed, true);
  assert.equal(items[5].failed, undefined, 'ftp is not checked, and not reported as failed');
  assert.equal(requests.filter((r) => r.endsWith('/a.zip')).length, 1);
  assert.ok(requests.includes('GET https://a.test/nohead.zip'));
  assert.ok(requests.includes('GET https://a.test/down.zip'), 'a refused HEAD is retried as GET');
});

test('stopping the checker aborts requests and sends nothing more', async () => {
  let aborted = 0;
  const fetchFn = (url, init) => new Promise((resolve, reject) => {
    init.signal.addEventListener('abort', () => {
      aborted += 1;
      reject(new DOMException('aborted', 'AbortError'));
    });
  });
  const batches = [];
  const checker = new LinkChecker(fetchFn, (items) => batches.push(items));
  const running = checker.run(Array.from({ length: 20 }, (_, i) => `https://a.test/${i}`));
  await new Promise((resolve) => setImmediate(resolve));
  checker.stop();
  await running;
  assert.equal(aborted, 6, 'one request per worker was in flight');
  assert.deepEqual(batches, []);
});

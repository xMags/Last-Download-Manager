import assert from 'node:assert/strict';
import { test } from 'node:test';

import { MediaDetector } from '../src/media-detect.js';
import { classifyMedia, mediaRules, replacesPage, responseSize } from '../src/media-rules.js';
import { makeMedia, makeMediaReset, makeVideoPosition, parseConfig, parseMediaList } from '../src/protocol.js';
import { chooseVideo, cleanFrameReport, VideoPositions } from '../src/video-positions.js';

const rules = mediaRules({ media: { enabled: true, types: ['mp4', 'webm', 'mp3', 'ts', 'vtt'], minimumBytes: 1000,
  siteExceptions: ['*.blocked.test'] } });
const headers = (contentType, length) => [{ name: 'Content-Type', value: contentType },
  ...(length === undefined ? [] : [{ name: 'content-length', value: String(length) }])];
const response = (url, contentType, length, statusCode = 200) => ({ url, statusCode, responseHeaders: headers(contentType, length) });

test('manifests are found by content type or by extension', () => {
  assert.deepEqual(classifyMedia(response('https://a.test/x', 'application/vnd.apple.mpegurl'), undefined, rules, false),
    { kind: 'hls', mimeType: 'application/vnd.apple.mpegurl' });
  assert.equal(classifyMedia(response('https://a.test/live/index.m3u8?t=1', 'text/plain'), undefined, rules, false).kind, 'hls');
  assert.equal(classifyMedia(response('https://a.test/x', 'application/dash+xml; charset=utf-8'), undefined, rules, false).kind,
    'dash');
  assert.equal(classifyMedia(response('https://a.test/manifest.mpd', ''), undefined, rules, false).kind, 'dash');
});

test('plain files need a listed type and the minimum size', () => {
  assert.deepEqual(classifyMedia(response('https://a.test/v', 'video/mp4', 5000), undefined, rules, false),
    { kind: 'file', mimeType: 'video/mp4', fileSize: 5000 });
  assert.equal(classifyMedia(response('https://a.test/clip.webm', '', 2000), undefined, rules, false).kind, 'file');
  assert.equal(classifyMedia(response('https://a.test/v', 'video/mp4', 999), undefined, rules, false), null, 'too small');
  assert.equal(classifyMedia(response('https://a.test/v.mkv', 'video/x-matroska', 5000), undefined, rules, false), null,
    'not a listed type');
  assert.equal(classifyMedia(response('https://a.test/v', 'video/mp4'), undefined, rules, false).kind, 'file',
    'an unknown size is offered');
  assert.equal(classifyMedia(response('https://a.test/v', 'text/html', 5000), undefined, rules, false), null);
});

test('responses that are not media for the panel are left out', () => {
  assert.equal(classifyMedia(response('https://a.test/v.mp4', 'video/mp4', 5000, 404), undefined, rules, false), null);
  assert.equal(classifyMedia(response('ftp://a.test/v.mp4', 'video/mp4', 5000), undefined, rules, false), null);
  assert.equal(classifyMedia(response('https://cdn.test/v.mp4', 'video/mp4', 5000), 'https://www.blocked.test/watch', rules,
    false), null, 'the page is a site exception');
  assert.equal(classifyMedia(response('https://a.test/m.m3u8', ''), undefined, { ...rules, enabled: false }, false), null);
  assert.equal(classifyMedia(response('https://a.test/m.m3u8', ''), undefined, { ...rules, types: [] }, false), null,
    'no media types turns the panel off');
});

test('once a tab plays a stream, its pieces are not offered as files', () => {
  assert.equal(classifyMedia(response('https://a.test/seg001.ts', 'video/mp2t', 5000), undefined, rules, true), null);
  assert.equal(classifyMedia(response('https://a.test/subs.vtt', 'text/vtt', 5000), undefined, rules, true), null);
  assert.equal(classifyMedia(response('https://a.test/chunk', 'video/mp4', 5000, 206), undefined, rules, true), null);
  assert.equal(classifyMedia(response('https://a.test/seg001.ts', 'video/mp2t', 5000), undefined, rules, false).kind, 'file');
  assert.equal(classifyMedia(response('https://a.test/song.mp3', 'audio/mpeg', 5000), undefined, rules, true).kind, 'file');
});

test('sizes come from Content-Length, or from the total of a partial answer', () => {
  assert.equal(responseSize(headers('video/mp4', 42), 200), 42);
  assert.equal(responseSize([{ name: 'Content-Range', value: 'bytes 0-99/123456' }], 206), 123456);
  assert.equal(responseSize([{ name: 'Content-Range', value: 'bytes 0-99/*' }], 206), undefined);
  assert.equal(responseSize([{ name: 'Content-Length', value: '12x' }], 200), undefined);
});

test('config media rules are checked', () => {
  const config = parseConfig({ v: 1, type: 'config', appVersion: '5.0.0', capturedExtensions: [],
    media: { enabled: true, types: ['mp4', 'bad type', '.webm'], minimumBytes: -5, siteExceptions: ['ok.test', 'a b'] } });
  assert.deepEqual(config.media, { enabled: true, types: ['mp4', 'webm'], minimumBytes: 0, siteExceptions: ['ok.test'] });
  assert.deepEqual(mediaRules(parseConfig({ v: 1, type: 'config', appVersion: '5', capturedExtensions: [] })).types, [],
    'an LDM without media rules gets no reports');
});

test('media messages carry only what LDM accepts', () => {
  const message = makeMedia({ tabId: 4, frameId: 0, incognito: false, kind: 'hls', url: 'https://a.test/m.m3u8',
    pageUrl: 'https://a.test/watch', pageTitle: 'A\nfilm', mimeType: 'application/x-mpegurl', userAgent: 'UA',
    cookies: [{ domain: 'a.test', hostOnly: true, path: '/', secure: true, session: true, name: 'n', value: 'v' }] });
  assert.equal(message.type, 'media');
  assert.equal(message.pageTitle, 'A film');
  assert.equal(message.cookies.length, 1);
  assert.equal(makeMedia({ ...message, cookies: [], url: 'ftp://a.test/m.m3u8' }), null);
  assert.equal(makeMedia({ ...message, cookies: [], tabId: -1 }), null);
  assert.equal(makeMedia({ ...message, cookies: [], kind: 'rtmp' }), null);
  assert.equal(makeMedia({ ...message, cookies: [], pageUrl: 'javascript:x' }).pageUrl, undefined);
  assert.equal(makeMediaReset(4).tabId, 4);
  assert.equal(makeMediaReset(-1), null);
});

test('positions are checked and rounded', () => {
  const rect = { x: 1.234, y: 2, width: 640, height: 360 };
  const position = makeVideoPosition({ tabId: 1, visible: true, fullscreen: false, window: rect, viewport: rect,
    viewportPositionKnown: false, video: rect, pixelRatio: 1.5 });
  assert.equal(position.video.x, 1.2);
  assert.equal(makeVideoPosition({ ...position, pixelRatio: 0 }), null);
  assert.equal(makeVideoPosition({ ...position, video: { ...rect, width: -1 } }), null);
  assert.equal(makeVideoPosition({ ...position, video: { ...rect, x: Infinity } }), null);
});

test('media-list answers are checked', () => {
  assert.deepEqual(parseMediaList({ v: 1, type: 'media-list', tabId: 3, hasMedia: true }), { tabId: 3, hasMedia: true });
  assert.equal(parseMediaList({ v: 1, type: 'media-list', tabId: '3', hasMedia: true }), null);
  assert.equal(parseMediaList({ v: 1, type: 'media-list', tabId: 3 }), null);
});

test('the best video of any frame is chosen, with the top frame viewport', () => {
  const now = 100000;
  const top = { report: { video: { x: 0, y: 0, width: 200, height: 100 }, score: 20000, viewport: { x: 0, y: 0, width: 1000,
    height: 800 } }, time: now };
  const frame = { report: { video: { x: 50, y: 60, width: 640, height: 360 }, score: 900000 }, time: now };
  const stale = { report: { video: { x: 0, y: 0, width: 1000, height: 800 }, score: 9e9 }, time: now - 60000 };
  const chosen = chooseVideo(new Map([[0, top], [3, frame], [4, stale]]), now);
  assert.deepEqual(chosen.video, frame.report.video);
  assert.equal(chosen.top, top.report);
  assert.equal(chooseVideo(new Map([[3, frame]]), now).top, null, 'no top frame report: no viewport');
});

test('frame reports are checked; only the top frame brings the viewport', () => {
  const report = { type: 'video-rect', video: { x: 1, y: 2, width: 3, height: 4 }, score: 12, hidden: false,
    fullscreen: false, viewport: { x: 0, y: 0, width: 10, height: 10 }, pixelRatio: 2 };
  assert.deepEqual(cleanFrameReport(report, false), { video: { x: 1, y: 2, width: 3, height: 4 }, score: 12, hidden: false,
    fullscreen: false });
  assert.equal(cleanFrameReport(report, true).pixelRatio, 2);
  assert.equal(cleanFrameReport({ ...report, video: { x: 'a' } }, false), null);
  assert.equal(cleanFrameReport({ ...report, viewport: undefined }, true), null);
  assert.equal(cleanFrameReport({ ...report, type: 'other' }, false), null);
});

function fakeApi({ tab = { id: 7, url: 'https://a.test/watch', title: 'Watch', incognito: false, active: true, windowId: 1 } } = {}) {
  const sent = [];
  return {
    sent,
    api: {
      tabs: { get: async () => tab, sendMessage: async (...args) => sent.push(['tab', ...args]) },
      windows: { get: async () => ({ left: 0, top: 0, width: 1200, height: 900, state: 'normal' }) },
      cookies: { getAll: async () => [{ domain: 'a.test', hostOnly: true, path: '/', secure: true, session: true, name: 'n', value: 'v' }],
        getAllCookieStores: async () => [] },
    },
    connection: { post: (message) => { sent.push(['post', message]); return true; } },
  };
}

test('each media address of a tab is reported once, and a reset forgets them', async () => {
  const { api, connection, sent } = fakeApi();
  const detector = new MediaDetector(api, connection, { ready: Promise.resolve(), isEnabled: () => true,
    rules: () => ({ media: { types: ['mp4'] } }), userAgent: 'UA' });
  const details = { tabId: 7, frameId: 0, type: 'xmlhttprequest', ...response('https://cdn.a.test/m.m3u8', '') };
  await Promise.all([detector.onHeadersReceived(details), detector.onHeadersReceived(details)]);
  const reports = sent.filter(([kind]) => kind === 'post');
  assert.equal(reports.length, 1, 'two responses for one address at once give one report');
  assert.equal(reports[0][1].kind, 'hls');
  assert.equal(reports[0][1].pageTitle, 'Watch');
  assert.equal(reports[0][1].cookies.length, 1);

  await detector.onHeadersReceived({ ...details, tabId: -1 });
  await detector.onHeadersReceived({ ...details, type: 'image', url: 'https://cdn.a.test/other.m3u8' });
  assert.equal(sent.filter(([kind]) => kind === 'post').length, 1, 'requests of no tab and pictures are ignored');

  detector.reset(7);
  assert.equal(sent.at(-1)[1].type, 'media-reset');
  await detector.onHeadersReceived(details);
  assert.equal(sent.at(-1)[1].type, 'media', 'reported again after the page changed');
});

test('nothing is reported while integration is off', async () => {
  const { api, connection, sent } = fakeApi();
  const detector = new MediaDetector(api, connection, { ready: Promise.resolve(), isEnabled: () => false,
    rules: () => ({ media: { types: ['mp4'] } }), userAgent: 'UA' });
  await detector.onHeadersReceived({ tabId: 7, frameId: 0, type: 'media', ...response('https://a.test/m.m3u8', '') });
  assert.equal(sent.length, 0);
});

test('positions go to LDM only for tracked tabs, and only when they change', async () => {
  const { api, connection, sent } = fakeApi();
  const positions = new VideoPositions(api, connection);
  const sender = { tab: { id: 7 }, frameId: 0 };
  const report = { type: 'video-rect', video: { x: 10, y: 20, width: 640, height: 360 }, score: 5, hidden: false,
    fullscreen: false, viewport: { x: 0, y: 0, width: 1200, height: 800 }, viewportPositionKnown: false, pixelRatio: 1 };
  positions.onFrameReport(report, sender);
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(sent.filter(([kind]) => kind === 'post').length, 0, 'not tracked yet');

  positions.setHasMedia(7, true);
  assert.deepEqual(sent.at(-1), ['tab', 7, { type: 'ldm-track-video', on: true }]);
  positions.onFrameReport(report, sender);
  await new Promise((resolve) => setTimeout(resolve, 0));
  positions.onFrameReport(report, sender);
  await new Promise((resolve) => setTimeout(resolve, 0));
  const posted = sent.filter(([kind]) => kind === 'post').map(([, message]) => message);
  assert.equal(posted.length, 1, 'the same position is sent once');
  assert.equal(posted[0].type, 'video-position');
  assert.equal(posted[0].visible, true);
  assert.deepEqual(posted[0].window, { x: 0, y: 0, width: 1200, height: 900 });

  positions.setHasMedia(7, false);
  assert.deepEqual(sent.at(-1), ['tab', 7, { type: 'ldm-track-video', on: false }]);
});

test('only an answer that replaces the page clears the tab\'s media', () => {
  const page = (statusCode, headers = {}, extra = {}) => replacesPage({
    statusCode, responseHeaders: Object.entries(headers).map(([name, value]) => ({ name, value })), ...extra,
  });
  assert.equal(page(200, { 'Content-Type': 'text/html; charset=utf-8' }), true);
  assert.equal(page(200), true, 'the browser looks at the content');
  assert.equal(page(200, { 'Content-Type': 'video/mp4' }), true, 'a video shown in the tab');
  assert.equal(page(200, { 'Content-Type': 'application/zip' }), false, 'downloaded');
  assert.equal(page(200, { 'Content-Type': 'text/plain', 'Content-Disposition': 'attachment; filename=a.txt' }), false);
  assert.equal(page(204), false);
  assert.equal(page(302, { Location: '/x' }), false);
  assert.equal(page(200, { 'Content-Type': 'text/html' }, { documentLifecycle: 'prerender' }), false);
});

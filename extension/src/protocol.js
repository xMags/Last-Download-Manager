import { keySettings } from './key-rules.js';

// Messages between this extension and LDM (M3 design section 4). LDM validates everything it
// receives; this side validates what LDM sends, so a broken or foreign host cannot feed the
// extension nonsense.

export const PROTOCOL_VERSION = 1;
export const HOST_NAME = 'io.github.mrdevfx.ldm';

const MAX_NAME = 64;
const MAX_VERSION = 32;
// A pattern from Options > File types, lower case; LDM ignores a leading dot, and so does capture-rules.js.
const EXTENSION_PATTERN = /^\.?[a-z0-9*?_+~-]{1,32}$/;

let lastId = 0;

/** A fresh request id, unique while the extension runs. */
export function nextId() {
  lastId += 1;
  return `m${lastId}`;
}

/** Printable ASCII only, cut to `max` characters: LDM rejects anything else. */
export function cleanText(value, max) {
  return String(value ?? '').replace(/[^\x20-\x7e]/g, '').slice(0, max);
}

/**
 * The first message on every connection.
 * @param {{browser: string, browserVersion: string, extensionVersion: string,
 *          incognitoAllowed: boolean, startLdm: boolean}} info
 */
export function makeHello(info) {
  return {
    v: PROTOCOL_VERSION,
    id: nextId(),
    type: 'hello',
    browser: cleanText(info.browser, MAX_NAME) || 'Browser',
    browserVersion: cleanText(info.browserVersion, MAX_VERSION),
    extensionVersion: cleanText(info.extensionVersion, MAX_VERSION) || '0',
    incognitoAllowed: Boolean(info.incognitoAllowed),
    startLdm: Boolean(info.startLdm),
  };
}

const MAX_PATTERNS = 2000;
const MAX_PATTERN = 2048;

/** The valid patterns of an optional list: strings without spaces or control characters. */
function patternList(value) {
  if (!Array.isArray(value)) {
    return [];
  }
  return value.filter((entry) => typeof entry === 'string' && entry.length > 0 && entry.length <= MAX_PATTERN &&
    !/[\u0000-\u0020\u007f]/.test(entry)).slice(0, MAX_PATTERNS);
}

/**
 * The capture rules from a config message, or null when the message is not a valid config.
 * Invalid list entries are dropped rather than failing the whole config.
 * @returns {{appVersion: string, capturedExtensions: string[], siteExceptions: string[],
 *            addressExceptions: string[]} | null}
 */
export function parseConfig(message) {
  if (message === null || typeof message !== 'object' || message.v !== PROTOCOL_VERSION ||
      message.type !== 'config' || typeof message.appVersion !== 'string' ||
      !Array.isArray(message.capturedExtensions)) {
    return null;
  }
  return {
    appVersion: cleanText(message.appVersion, MAX_VERSION),
    capturedExtensions: message.capturedExtensions.filter(
      (entry) => typeof entry === 'string' && EXTENSION_PATTERN.test(entry)),
    siteExceptions: patternList(message.siteExceptions),
    addressExceptions: patternList(message.addressExceptions),
    keys: keySettings(message),
    menu: menuSettings(message),
    media: mediaSettings(message.media),
    // Site videos are on in LDM, which then takes site-page messages; older versions say nothing.
    sitePages: message.sitePages === true,
    // The sites whose pages LDM wants; LDM 5.1 sends no list and wants YouTube's.
    siteHosts: hostList(message.siteHosts),
    // The sites the user let sign in through the browser: their pages go with their cookies.
    signInHosts: hostList(message.signInHosts) ?? [],
  };
}

const MAX_SITE_HOSTS = 64;
const HOST_PATTERN = /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$/;

/** Lower-case host names with at least one dot; anything else is dropped. Null without a list. */
function hostList(value) {
  if (!Array.isArray(value)) {
    return null;
  }
  return value.filter((entry) => typeof entry === 'string' && entry.length <= 253 && HOST_PATTERN.test(entry))
    .slice(0, MAX_SITE_HOSTS);
}

/** The media panel's rules; an LDM that sends none gets no types, so nothing is reported. */
function mediaSettings(media) {
  const types = Array.isArray(media?.types) ? media.types : [];
  return {
    enabled: media?.enabled !== false,
    types: types.filter((entry) => typeof entry === 'string' && EXTENSION_PATTERN.test(entry))
      .map((entry) => entry.replace(/^\./, '')),
    minimumBytes: Number.isSafeInteger(media?.minimumBytes) && media.minimumBytes > 0 ? media.minimumBytes : 0,
    siteExceptions: patternList(media?.siteExceptions),
  };
}

/** Which right-click menu items LDM's Options turned on; both default to on, as in LDM. */
export function menuSettings(config) {
  const flag = (value) => value !== false;
  return { downloadLink: flag(config?.menu?.downloadLink), downloadAll: flag(config?.menu?.downloadAll) };
}

const MAX_URL = 16 * 1024;
const MAX_COOKIES = 300;
const CONTROL = /[\u0000-\u001f\u007f]/;
const HOST = /^[A-Za-z0-9_][A-Za-z0-9._-]{0,252}$/;
// An IPv6 address, as LDM keeps a host: without the brackets the browser may put around it.
const IPV6_HOST = /^[0-9A-Fa-f:.]{2,45}$/;

/** True for an address LDM accepts: http, https, ftp or ftps, without spaces or control characters. */
export function isDownloadAddress(url) {
  return typeof url === 'string' && url.length <= MAX_URL && !CONTROL.test(url) && !url.includes(' ') &&
    /^(https?|ftps?):\/\/./i.test(url);
}

/**
 * A browser cookie in the form LDM stores, or null when LDM would refuse it (the same rules as
 * model::IsValidCookie).
 */
export function toLdmCookie(cookie) {
  const domain = String(cookie.domain ?? '').replace(/^\./, '').replace(/^\[(.*)\]$/, '$1');
  const hostOnly = Boolean(cookie.hostOnly);
  const ipv6 = domain.includes(':');
  const result = {
    domain,
    hostOnly,
    path: String(cookie.path ?? '/'),
    secure: Boolean(cookie.secure),
    expires: cookie.session || !Number.isFinite(cookie.expirationDate) ? 0 : Math.max(0, Math.floor(cookie.expirationDate)),
    name: String(cookie.name ?? ''),
    value: String(cookie.value ?? ''),
  };
  // An address has no subdomains, so its cookies are always for that host only.
  const validHost = ipv6 ? hostOnly && IPV6_HOST.test(domain) : HOST.test(domain);
  const valid = validHost && result.path.startsWith('/') && result.path.length <= 2048 &&
    !CONTROL.test(result.path) && result.name.length > 0 && result.name.length <= 4096 && !CONTROL.test(result.name) &&
    result.value.length <= 8192 && !CONTROL.test(result.value);
  return valid ? result : null;
}

/**
 * The capture message for a browser download.
 * @param {{url: string, finalUrl?: string, referrer?: string, fileName?: string, fileSize?: number,
 *          mimeType?: string, userAgent: string, cookies: object[],
 *          post?: {contentType: string, body: string}, chosen?: boolean}} download post body is
 *   base64; chosen: the user picked LDM (right-click menu or force keys), so LDM does not count a
 *   cancel of it as a sign that the capture rules are wrong
 */
export function makeCapture(download) {
  const optionalAddress = (value) => (isDownloadAddress(value) ? value : undefined);
  const optionalText = (value, max) => {
    const text = String(value ?? '');
    // LDM's limits count UTF-8 bytes; one longer field would make it refuse the whole capture.
    return text && utf8Length(text) <= max && !CONTROL.test(text) ? text : undefined;
  };
  const seen = new Set();
  const cookies = [];
  for (const cookie of download.cookies) {
    const converted = toLdmCookie(cookie);
    const key = converted && `${converted.domain}\t${converted.path}\t${converted.name}`;
    if (converted && !seen.has(key) && cookies.length < MAX_COOKIES) {
      seen.add(key);
      cookies.push(converted);
    }
  }
  return {
    v: PROTOCOL_VERSION,
    id: nextId(),
    type: 'capture',
    url: download.url,
    finalUrl: download.finalUrl !== download.url ? optionalAddress(download.finalUrl) : undefined,
    referrer: optionalAddress(download.referrer),
    fileName: optionalText(download.fileName, 1024),
    fileSize: Number.isSafeInteger(download.fileSize) && download.fileSize > 0 ? download.fileSize : undefined,
    mimeType: optionalText(download.mimeType, 256),
    userAgent: cleanText(download.userAgent, 1024) || 'Mozilla/5.0',
    cookies,
    post: download.post && optionalText(download.post.contentType, 256)
      ? { contentType: download.post.contentType, body: download.post.body }
      : undefined,
    chosen: download.chosen === true ? true : undefined,
  };
}

/** True only for an answer that accepts the capture with the given id. */
export function isAcceptedCapture(message, id) {
  return message !== null && typeof message === 'object' && message.v === PROTOCOL_VERSION &&
    message.type === 'capture-result' && message.id === id && message.accepted === true;
}

// Limits of the links message, as LDM enforces them (browser/Protocol.h).
export const MAX_PAGE_LINKS = 5000;
const MAX_PAGE_LINK_COOKIES = 1000;
const MAX_LINK_TEXT_BYTES = 1024;
const MAX_FILE_NAME_BYTES = 1024;
const MAX_MIME_TYPE_BYTES = 256;
// LDM reads at most 4 MiB per message; this leaves room for the envelope and JSON escapes.
const MAX_LINKS_MESSAGE_BYTES = 3.5 * 1024 * 1024;
const MAX_LINK_COOKIE_BYTES = 1024 * 1024;
const LINK_KINDS = new Set(['link', 'image', 'frame', 'text']);

const encoder = new TextEncoder();

/** Bytes of `text` in UTF-8. */
export function utf8Length(text) {
  return encoder.encode(text).length;
}

/**
 * Page text made acceptable to LDM: lone surrogates (which no UTF-8 can hold) replaced, control
 * characters and runs of white space turned into one space, trimmed, and cut to `maxBytes` of
 * UTF-8 without splitting a character.
 */
export function cleanPageText(value, maxBytes) {
  const text = String(value ?? '')
    .replace(/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/g, '�')
    .replace(/[\s\u0000-\u001f\u007f]+/g, ' ')
    .trim();
  let bytes = 0;
  let end = 0;
  for (const character of text) {
    const size = character.codePointAt(0) < 0x80 ? 1 : utf8Length(character);
    if (bytes + size > maxBytes) {
      break;
    }
    bytes += size;
    end += character.length;
  }
  return text.slice(0, end).trimEnd();
}

/**
 * The links message for Download All Links or Download selected links (checklist B-05). Links
 * LDM would refuse are left out, and the list is cut so the message stays within LDM's limits.
 * @param {{selection: boolean, referrer?: string, userAgent: string,
 *          links: {url: string, text?: string, kind: string, fileName?: string}[],
 *          cookies: object[]}} page cookies as the browser's cookies API gives them
 * @returns {object | null} null when no link is left
 */
export function makeLinks(page) {
  const message = {
    v: PROTOCOL_VERSION,
    id: nextId(),
    type: 'links',
    selection: Boolean(page.selection),
    referrer: isDownloadAddress(page.referrer) ? page.referrer : undefined,
    userAgent: cleanText(page.userAgent, 1024) || 'Mozilla/5.0',
    links: [],
    cookies: [],
  };
  let budget = MAX_LINKS_MESSAGE_BYTES - utf8Length(JSON.stringify(message));

  const seen = new Set();
  let cookieBytes = 0;
  for (const cookie of page.cookies) {
    const converted = toLdmCookie(cookie);
    const key = converted && `${converted.domain}\t${converted.path}\t${converted.name}`;
    if (!converted || seen.has(key)) {
      continue;
    }
    const size = utf8Length(JSON.stringify(converted)) + 1;
    if (message.cookies.length >= MAX_PAGE_LINK_COOKIES || cookieBytes + size > MAX_LINK_COOKIE_BYTES) {
      break;
    }
    seen.add(key);
    cookieBytes += size;
    message.cookies.push(converted);
  }
  budget -= cookieBytes;

  for (const link of page.links) {
    if (message.links.length >= MAX_PAGE_LINKS) {
      break;
    }
    if (!isDownloadAddress(link?.url) || !LINK_KINDS.has(link.kind)) {
      continue;
    }
    const entry = { url: link.url, kind: link.kind };
    const text = cleanPageText(link.text, MAX_LINK_TEXT_BYTES);
    if (text) {
      entry.text = text;
    }
    const fileName = cleanPageText(link.fileName, MAX_FILE_NAME_BYTES);
    if (fileName) {
      entry.fileName = fileName;
    }
    const size = utf8Length(JSON.stringify(entry)) + 1;
    if (size > budget) {
      break;
    }
    budget -= size;
    message.links.push(entry);
  }
  return message.links.length > 0 ? message : null;
}

/** LDM's answer to a links message: true when the dialog opens, false when LDM declined it. */
export function linksAccepted(message, id) {
  return message !== null && typeof message === 'object' && message.v === PROTOCOL_VERSION &&
    message.type === 'links-result' && message.id === id && message.accepted === true;
}

/** True when LDM says the Download All Links dialog of request `id` closed. */
export function isLinksDone(message, id) {
  return message !== null && typeof message === 'object' && message.v === PROTOCOL_VERSION &&
    message.type === 'links-done' && message.id === id;
}

const MAX_PAGE_TITLE_BYTES = 1024;
const MEDIA_KINDS = new Set(['hls', 'dash', 'file']);

/** True for a tab or frame id LDM accepts. */
function isBrowserId(value) {
  return Number.isSafeInteger(value) && value >= 0 && value <= 0x7fffffff;
}

/** True for an http or https address LDM accepts. */
function isWebAddress(url) {
  return isDownloadAddress(url) && /^https?:/i.test(url);
}

/**
 * The media message for a response a tab loaded (M3 design section 6), or null when LDM would
 * refuse it.
 * @param {{tabId: number, frameId: number, incognito: boolean, kind: string, url: string,
 *          pageUrl?: string, pageTitle?: string, mimeType?: string, fileSize?: number,
 *          userAgent: string, cookies: object[]}} media cookies as the browser's cookies API gives them
 */
export function makeMedia(media) {
  if (!isBrowserId(media.tabId) || !isBrowserId(media.frameId) || !MEDIA_KINDS.has(media.kind) ||
      !isWebAddress(media.url)) {
    return null;
  }
  const seen = new Set();
  const cookies = [];
  for (const cookie of media.cookies) {
    const converted = toLdmCookie(cookie);
    const key = converted && `${converted.domain}\t${converted.path}\t${converted.name}`;
    if (converted && !seen.has(key) && cookies.length < MAX_COOKIES) {
      seen.add(key);
      cookies.push(converted);
    }
  }
  const mimeType = String(media.mimeType ?? '');
  return {
    v: PROTOCOL_VERSION,
    id: nextId(),
    type: 'media',
    tabId: media.tabId,
    frameId: media.frameId,
    incognito: Boolean(media.incognito),
    kind: media.kind,
    url: media.url,
    pageUrl: isWebAddress(media.pageUrl) ? media.pageUrl : undefined,
    pageTitle: cleanPageText(media.pageTitle, MAX_PAGE_TITLE_BYTES) || undefined,
    mimeType: mimeType && utf8Length(mimeType) <= 256 && !CONTROL.test(mimeType) ? mimeType : undefined,
    fileSize: Number.isSafeInteger(media.fileSize) && media.fileSize >= 0 ? media.fileSize : undefined,
    userAgent: cleanText(media.userAgent, 1024) || 'Mozilla/5.0',
    cookies,
  };
}

/**
 * The address of a tab on a supported video site (video-sites design, section 7), or null when LDM
 * would refuse it. Sent only to an LDM whose config asked for them: an older one ends the
 * connection on a message type it does not know.
 * @param {{tabId: number, incognito: boolean, url: string, pageTitle?: string, userAgent: string}} page
 */
export function makeSitePage(page) {
  if (!isBrowserId(page.tabId) || !isWebAddress(page.url)) {
    return null;
  }
  return {
    v: PROTOCOL_VERSION,
    id: nextId(),
    type: 'site-page',
    tabId: page.tabId,
    incognito: Boolean(page.incognito),
    url: page.url,
    pageTitle: cleanPageText(page.pageTitle, MAX_PAGE_TITLE_BYTES) || undefined,
    userAgent: cleanText(page.userAgent, 1024) || 'Mozilla/5.0',
    cookies: Array.isArray(page.cookies) ? cookieList(page.cookies) : undefined,
  };
}

/**
 * The user asked for the video of a page (the right-click menu's "Download video"): LDM asks its
 * site resolver about `url`. Only sent to an LDM that sent its list of sites.
 */
export function makeSiteRequest(request) {
  if (!isWebAddress(request.url)) {
    return null;
  }
  return {
    v: PROTOCOL_VERSION,
    id: nextId(),
    type: 'site-request',
    url: request.url,
    referrer: isWebAddress(request.referrer) ? request.referrer : undefined,
    incognito: Boolean(request.incognito),
    pageTitle: cleanPageText(request.pageTitle, MAX_PAGE_TITLE_BYTES) || undefined,
    userAgent: cleanText(request.userAgent, 1024) || 'Mozilla/5.0',
    cookies: Array.isArray(request.cookies) ? cookieList(request.cookies) : undefined,
  };
}

/** The tab's page changed or the tab closed. */
export function makeMediaReset(tabId) {
  return isBrowserId(tabId) ? { v: PROTOCOL_VERSION, id: nextId(), type: 'media-reset', tabId } : null;
}

const MAX_COORDINATE = 1000000;

/** A rectangle LDM accepts, rounded to a tenth of a pixel; null for anything else. */
function cleanRect(rect) {
  const values = [rect?.x, rect?.y, rect?.width, rect?.height];
  if (!values.every((value) => Number.isFinite(value) && Math.abs(value) <= MAX_COORDINATE) ||
      rect.width < 0 || rect.height < 0) {
    return null;
  }
  const round = (value) => Math.round(value * 10) / 10;
  return { x: round(rect.x), y: round(rect.y), width: round(rect.width), height: round(rect.height) };
}

/**
 * Where a tab's video is, for LDM's panel; null when a value is out of range.
 * @param {{tabId: number, visible: boolean, fullscreen: boolean, window: object, viewport: object,
 *          viewportPositionKnown: boolean, video: object, pixelRatio: number}} position
 */
export function makeVideoPosition(position) {
  const window = cleanRect(position.window);
  const viewport = cleanRect(position.viewport);
  const video = cleanRect(position.video);
  const ratio = position.pixelRatio;
  if (!isBrowserId(position.tabId) || !window || !viewport || !video || !Number.isFinite(ratio) || ratio < 0.1 ||
      ratio > 16) {
    return null;
  }
  return {
    v: PROTOCOL_VERSION,
    id: nextId(),
    type: 'video-position',
    tabId: position.tabId,
    visible: Boolean(position.visible),
    fullscreen: Boolean(position.fullscreen),
    window,
    viewport,
    viewportPositionKnown: Boolean(position.viewportPositionKnown),
    video,
    pixelRatio: ratio,
  };
}

/** LDM says whether it offers something for a tab: {tabId, hasMedia}, or null for anything else. */
export function parseMediaList(message) {
  if (message === null || typeof message !== 'object' || message.v !== PROTOCOL_VERSION ||
      message.type !== 'media-list' || !isBrowserId(message.tabId) || typeof message.hasMedia !== 'boolean') {
    return null;
  }
  return { tabId: message.tabId, hasMedia: message.hasMedia };
}

const MAX_REQUEST_ID = 64;

/**
 * LDM's request for the cookies the browser sends to an address (the Site Grabber), or null when
 * the message is not a valid one.
 * @returns {{requestId: string, url: string} | null}
 */
export function parseCookiesRequest(message) {
  if (message === null || typeof message !== 'object' || message.v !== PROTOCOL_VERSION ||
      message.type !== 'cookies-request' || typeof message.id !== 'string' || message.id.length === 0 ||
      message.id.length > MAX_REQUEST_ID || !/^[\x20-\x7e]+$/.test(message.id) || !isDownloadAddress(message.url) ||
      !/^https?:/i.test(message.url)) {
    return null;
  }
  return { requestId: message.id, url: message.url };
}

/** Browser cookies as LDM accepts them: converted, without repeats, at most MAX_COOKIES. */
export function cookieList(browserCookies) {
  const seen = new Set();
  const cookies = [];
  for (const cookie of browserCookies ?? []) {
    const converted = toLdmCookie(cookie);
    const key = converted && `${converted.domain}\t${converted.path}\t${converted.name}`;
    if (converted && !seen.has(key) && cookies.length < MAX_COOKIES) {
      seen.add(key);
      cookies.push(converted);
    }
  }
  return cookies;
}

/** The answer to a cookies request: the browser's cookies for its address, as LDM accepts them. */
export function makeCookies(requestId, browserCookies) {
  return { v: PROTOCOL_VERSION, id: nextId(), type: 'cookies', requestId, cookies: cookieList(browserCookies) };
}

/**
 * What was found out about some links, for the open Download All Links dialog.
 * @param {{index: number, failed?: boolean, fileSize?: number, mimeType?: string,
 *          fileName?: string}[]} items
 */
export function makeLinkInfo(items) {
  const optionalText = (value, maxBytes) => {
    const text = cleanPageText(value, maxBytes);
    return text || undefined;
  };
  return {
    v: PROTOCOL_VERSION,
    id: nextId(),
    type: 'link-info',
    items: items.filter((item) => Number.isSafeInteger(item.index) && item.index >= 0 && item.index < MAX_PAGE_LINKS)
      .map((item) => (item.failed ? { index: item.index, failed: true } : {
        index: item.index,
        fileSize: Number.isSafeInteger(item.fileSize) && item.fileSize >= 0 ? item.fileSize : undefined,
        mimeType: optionalText(item.mimeType, MAX_MIME_TYPE_BYTES),
        fileName: optionalText(item.fileName, MAX_FILE_NAME_BYTES),
      })),
  };
}

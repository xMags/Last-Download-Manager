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
  const domain = String(cookie.domain ?? '').replace(/^\./, '');
  const result = {
    domain,
    hostOnly: Boolean(cookie.hostOnly),
    path: String(cookie.path ?? '/'),
    secure: Boolean(cookie.secure),
    expires: cookie.session || !Number.isFinite(cookie.expirationDate) ? 0 : Math.max(0, Math.floor(cookie.expirationDate)),
    name: String(cookie.name ?? ''),
    value: String(cookie.value ?? ''),
  };
  const valid = HOST.test(domain) && result.path.startsWith('/') && result.path.length <= 2048 &&
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
    return text && text.length <= max && !CONTROL.test(text) ? text : undefined;
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

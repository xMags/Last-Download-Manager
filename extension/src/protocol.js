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
  };
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
 *          mimeType?: string, userAgent: string, cookies: object[]}} download
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
  };
}

/** True only for an answer that accepts the capture with the given id. */
export function isAcceptedCapture(message, id) {
  return message !== null && typeof message === 'object' && message.v === PROTOCOL_VERSION &&
    message.type === 'capture-result' && message.id === id && message.accepted === true;
}

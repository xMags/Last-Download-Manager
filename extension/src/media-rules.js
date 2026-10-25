import { extensionOf, matchesSiteList, nameFromUrl } from './capture-rules.js';

// Which responses are media for LDM's panel over web players (checklists B-07, B-09; M3 design
// section 6). The extension only looks at responses as they arrive; LDM fetches and reads the
// manifests itself.

const HLS_TYPES = new Set(['application/vnd.apple.mpegurl', 'application/x-mpegurl', 'audio/mpegurl',
  'audio/x-mpegurl']);
const DASH_TYPES = new Set(['application/dash+xml']);

// Content types of plain media files, by the extension the panel's type list uses.
const FILE_TYPES = new Map([
  ['video/mp4', 'mp4'], ['video/webm', 'webm'], ['audio/webm', 'webm'], ['video/x-matroska', 'mkv'],
  ['video/quicktime', 'mov'], ['video/x-flv', 'flv'], ['video/x-f4v', 'f4v'], ['video/3gpp', '3gp'],
  ['video/x-msvideo', 'avi'], ['video/x-ms-wmv', 'wmv'], ['video/x-ms-asf', 'asf'], ['video/mpeg', 'mpeg'],
  ['video/ogg', 'ogv'], ['video/mp2t', 'ts'], ['video/x-m4v', 'm4v'], ['audio/mpeg', 'mp3'], ['audio/mp4', 'm4a'],
  ['audio/x-m4a', 'm4a'], ['audio/aac', 'aac'], ['audio/ogg', 'ogg'], ['audio/wav', 'wav'], ['audio/x-wav', 'wav'],
  ['audio/x-ms-wma', 'wma'], ['application/vnd.rn-realmedia', 'rm'], ['text/vtt', 'vtt'],
  ['application/ttml+xml', 'ttml'],
]);

// Once a tab plays a stream, these are its pieces, not files of their own.
const SEGMENT_EXTENSIONS = new Set(['ts', 'm4s', 'aac', 'vtt', 'webvtt', 'm4a', 'm4v', 'mp4', 'cmfv', 'cmfa']);

/**
 * The panel's rules from the cached config (already checked by parseConfig). An LDM that sends
 * none gets no reports: without types nothing is media.
 */
export function mediaRules(config) {
  const media = config?.media;
  return {
    enabled: media?.enabled !== false,
    types: Array.isArray(media?.types) ? media.types : [],
    minimumBytes: Number.isSafeInteger(media?.minimumBytes) ? media.minimumBytes : 0,
    siteExceptions: Array.isArray(media?.siteExceptions) ? media.siteExceptions : [],
  };
}

/** A response header's value, case-insensitively by name. */
function header(headers, name) {
  const found = (headers ?? []).find((entry) => String(entry.name).toLowerCase() === name);
  return found ? String(found.value ?? '') : '';
}

// Types a browser shows in the tab instead of downloading them.
const SHOWN_TYPE = /^(text\/|image\/|video\/|audio\/|application\/(xhtml\+xml|xml|pdf|json)$)/;

/**
 * True when a tab's main-frame response replaces its page, so what the old page loaded is gone.
 * Not for a page Chrome prerenders (it may never be shown), an empty answer (204, 205), a redirect
 * (the next response decides) or a file the browser downloads, which leave the page as it was.
 * @param {{statusCode: number, documentLifecycle?: string, responseHeaders?: {name: string, value?: string}[]}} response
 */
export function replacesPage(response) {
  if (response.documentLifecycle === 'prerender') {
    return false;
  }
  const status = response.statusCode;
  if (status === 204 || status === 205 || (status >= 300 && status < 400)) {
    return false;
  }
  if (/^\s*attachment/i.test(header(response.responseHeaders, 'content-disposition'))) {
    return false;
  }
  const type = header(response.responseHeaders, 'content-type').split(';')[0].trim().toLowerCase();
  return type === '' || SHOWN_TYPE.test(type);  // without a type the browser looks at the content
}

/** The response's whole size: Content-Length, or the total of a Content-Range. */
export function responseSize(headers, statusCode) {
  if (statusCode === 206) {
    const total = /\/(\d+)\s*$/.exec(header(headers, 'content-range'));
    return total ? Number(total[1]) : undefined;
  }
  const length = header(headers, 'content-length').trim();
  return /^\d+$/.test(length) ? Number(length) : undefined;
}

/**
 * What a response is for the panel, or null when it is nothing to offer.
 * @param {{url: string, statusCode: number, responseHeaders?: {name: string, value?: string}[]}} response
 * @param {string | undefined} pageUrl the tab's page, for the site exceptions
 * @param {{enabled: boolean, types: string[], minimumBytes: number, siteExceptions: string[]}} rules
 * @param {boolean} streamSeen the tab already loaded a manifest, so its media pieces are segments
 * @returns {{kind: 'hls' | 'dash' | 'file', mimeType?: string, fileSize?: number} | null}
 */
export function classifyMedia(response, pageUrl, rules, streamSeen) {
  if (!rules.enabled || rules.types.length === 0 || !/^https?:\/\//i.test(response.url)) {
    return null;
  }
  if (response.statusCode !== 200 && response.statusCode !== 206) {
    return null;
  }
  if (matchesSiteList(rules.siteExceptions, pageUrl ?? response.url)) {
    return null;
  }
  const mimeType = header(response.responseHeaders, 'content-type').split(';')[0].trim().toLowerCase() || undefined;
  const extension = extensionOf(nameFromUrl(response.url));
  if (HLS_TYPES.has(mimeType) || extension === 'm3u8') {
    return { kind: 'hls', mimeType };
  }
  if (DASH_TYPES.has(mimeType) || extension === 'mpd') {
    return { kind: 'dash', mimeType };
  }
  const type = FILE_TYPES.get(mimeType) ?? extension;
  if (!type || !rules.types.includes(type)) {
    return null;
  }
  // Pieces of a stream: parts of the tab's manifest, or byte ranges a player asks for one by one.
  if (streamSeen && (SEGMENT_EXTENSIONS.has(type) || SEGMENT_EXTENSIONS.has(extension) || response.statusCode === 206)) {
    return null;
  }
  const fileSize = responseSize(response.responseHeaders, response.statusCode);
  if (fileSize !== undefined && fileSize < rules.minimumBytes) {
    return null;
  }
  return { kind: 'file', mimeType, fileSize };
}

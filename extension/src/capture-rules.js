// Which downloads LDM takes over (checklist B-02). Mirrors LDM's own matching in
// src/core/Matching.cpp and FileNames.cpp; extension/test/capture-cases.json is checked by both
// test suites so the two cannot drift apart.

const SCHEMES = ['http:', 'https:', 'ftp:', 'ftps:'];

/** The extension after the last dot, lower case; empty when there is none or the name starts with it. */
export function extensionOf(name) {
  const dot = name.lastIndexOf('.');
  if (dot <= 0 || dot === name.length - 1) {
    return '';
  }
  return name.slice(dot + 1).replace(/[A-Z]/g, (c) => c.toLowerCase());
}

/** Case-insensitive match where '*' stands for any run of characters, including none. */
export function wildcardMatch(pattern, text) {
  let p = 0;
  let t = 0;
  let starPattern = -1;
  let starText = 0;
  const lower = (c) => c.replace(/[A-Z]/g, (u) => u.toLowerCase());
  while (t < text.length) {
    if (p < pattern.length && pattern[p] === '*') {
      starPattern = p;
      p += 1;
      starText = t;
    } else if (p < pattern.length && lower(pattern[p]) === lower(text[t])) {
      p += 1;
      t += 1;
    } else if (starPattern !== -1) {
      p = starPattern + 1;
      starText += 1;
      t = starText;
    } else {
      return false;
    }
  }
  while (p < pattern.length && pattern[p] === '*') {
    p += 1;
  }
  return p === pattern.length;
}

/** True when the file name's extension is in the list of patterns from LDM's Options. */
export function matchesExtensionList(patterns, name) {
  const extension = extensionOf(name);
  if (!extension) {
    return false;
  }
  return patterns.some((entry) => wildcardMatch(entry.startsWith('.') ? entry.slice(1) : entry, extension));
}

/** The last path segment of an address, decoded when possible. */
export function nameFromUrl(url) {
  try {
    const path = new URL(url).pathname;
    const last = path.slice(path.lastIndexOf('/') + 1);
    try {
      return decodeURIComponent(last);
    } catch {
      return last;
    }
  } catch {
    return '';
  }
}

/** The file name without any folder, from a browser path such as Firefox's full target path. */
export function baseName(path) {
  return String(path ?? '').split(/[\\/]/).pop();
}

/**
 * True when the address is on a site of the list. A pattern with a '/' is matched against the host
 * and path, any other against the host alone, as LDM matches category sites.
 */
export function matchesSiteList(sites, url) {
  let parsed;
  try {
    parsed = new URL(url);
  } catch {
    return false;
  }
  return sites.some((site) => wildcardMatch(site, site.includes('/') ? parsed.hostname + parsed.pathname : parsed.hostname));
}

/** True when the whole address matches one of the patterns. */
export function matchesAddressList(addresses, url) {
  return addresses.some((pattern) => wildcardMatch(pattern, url));
}

/** True for an address of a scheme LDM downloads. */
export function isCapturableAddress(url) {
  try {
    return SCHEMES.includes(new URL(url).protocol);
  } catch {
    return false;
  }
}

/**
 * Whether to take a browser download over (checklist B-02).
 * @param {{url: string, finalUrl?: string, fileName: string}} download fileName may be empty
 * @param {{capturedExtensions: string[], siteExceptions?: string[], addressExceptions?: string[]}} rules
 */
export function shouldCapture({ url, finalUrl, fileName }, rules) {
  let scheme;
  try {
    scheme = new URL(url).protocol;
  } catch {
    return false;
  }
  if (!SCHEMES.includes(scheme)) {
    return false;  // blob:, data: and file: downloads only exist inside the browser
  }
  // An exception on either the link or where it led keeps the download in the browser.
  const addresses = finalUrl && finalUrl !== url ? [url, finalUrl] : [url];
  if (addresses.some((address) => matchesSiteList(rules.siteExceptions ?? [], address) ||
                                 matchesAddressList(rules.addressExceptions ?? [], address))) {
    return false;
  }
  return matchesExtensionList(rules.capturedExtensions, fileName || nameFromUrl(finalUrl || url));
}

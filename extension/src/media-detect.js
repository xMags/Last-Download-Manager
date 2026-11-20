import { cookieStoreFor } from './cookie-stores.js';
import { classifyMedia, mediaRules } from './media-rules.js';
import { isDownloadAddress, makeMedia, makeMediaReset } from './protocol.js';

// Reports the media each tab loads to LDM (M3 design section 6), once per address, and tells LDM
// when a tab's page changes or the tab closes. Reports go over the lasting connection only, so
// browsing never starts LDM.

// Requests of these kinds are never media.
const IGNORED_TYPES = new Set(['image', 'stylesheet', 'script', 'font', 'ping', 'csp_report', 'beacon', 'imageset']);
// A page that loads more media than this is not one whose videos the panel can offer sensibly.
const MAX_REPORTS_PER_TAB = 200;
// When a single-page site moves to another page, media reported this shortly before belongs to
// the new page: its player may ask for the video just before the site changes the address.
const NEW_PAGE_GRACE_MS = 3000;

/**
 * What tells one page of a site from another: the address without its query and fragment. Sites
 * rewrite those while one video plays (a start time, a tracking tag), so only a new path is a new
 * page. Null for an address that is not http(s).
 */
export function pageKey(url) {
  try {
    const address = new URL(url);
    return /^https?:$/.test(address.protocol) ? address.origin + address.pathname : null;
  } catch {
    return null;
  }
}

export class MediaDetector {
  /**
   * @param {object} api the extension API object
   * @param {{post: function(object): boolean}} connection the lasting connection to LDM
   * @param {{ready: Promise, isEnabled: function(): boolean, rules: function(): object, userAgent: string,
   *          now?: function(): number}} options
   */
  constructor(api, connection, options) {
    this.api = api;
    this.connection = connection;
    this.options = options;
    this.now = options.now ?? (() => Date.now());
    // tab id -> {reported: Set of addresses, streamSeen: boolean, page: pageKey of the tab's address,
    //            reports: [{time, media}] (what was sent, to send again for a new page)}
    this.tabs = new Map();
  }

  /** webRequest.onHeadersReceived, observing only. */
  async onHeadersReceived(details) {
    if (!Number.isSafeInteger(details.tabId) || details.tabId < 0 || IGNORED_TYPES.has(details.type)) {
      return;
    }
    await this.options.ready;
    if (!this.options.isEnabled()) {
      return;
    }
    const tab = await this.api.tabs.get(details.tabId).catch(() => null);
    if (!tab) {
      return;
    }
    const state = this.stateOf(details.tabId);
    if (state.reported.has(details.url) || state.reported.size >= MAX_REPORTS_PER_TAB) {
      return;
    }
    const pageUrl = isDownloadAddress(tab.url) ? tab.url : undefined;
    const found = classifyMedia(details, pageUrl, mediaRules(this.options.rules()), state.streamSeen);
    if (!found) {
      return;
    }
    // Marked before the awaits below, so a second response for the same address is not reported twice.
    state.reported.add(details.url);
    state.page ??= pageKey(tab.url);
    if (found.kind !== 'file') {
      state.streamSeen = true;
    }
    const storeId = await cookieStoreFor(this.api, tab);
    const cookies = storeId === null ? [] : await this.api.cookies.getAll({ url: details.url, ...(storeId ? { storeId } : {}) })
      .catch(() => []);
    const media = {
      tabId: details.tabId,
      frameId: Number.isSafeInteger(details.frameId) && details.frameId >= 0 ? details.frameId : 0,
      incognito: Boolean(tab.incognito),
      kind: found.kind,
      url: details.url,
      pageUrl,
      pageTitle: tab.title,
      mimeType: found.mimeType,
      fileSize: found.fileSize,
      userAgent: this.options.userAgent,
      cookies,
    };
    if (this.tabs.get(details.tabId) !== state) {
      return;  // the tab loaded another page while the cookies were read
    }
    state.reports.push({ time: this.now(), media });
    this.connection.post(makeMedia(media));
  }

  /**
   * tabs.onUpdated with a new address. A single-page site (a video gallery, a feed) shows another
   * page without loading one, so what the old page played would stay offered beside the new
   * page's video. On a new path LDM starts the tab's media again, with what was reported in the
   * last moments, which belongs to the new page.
   */
  addressChanged(tabId, url) {
    const state = this.tabs.get(tabId);
    const page = pageKey(url);
    if (!state || page === null) {
      return;
    }
    if (state.page === null || state.page === undefined) {
      state.page = page;
      return;
    }
    if (state.page === page) {
      return;
    }
    const now = this.now();
    const recent = state.reports.filter((report) => now - report.time <= NEW_PAGE_GRACE_MS);
    state.page = page;
    state.reports = recent;
    state.reported = new Set(recent.map((report) => report.media.url));
    state.streamSeen = recent.some((report) => report.media.kind !== 'file');
    this.connection.post(makeMediaReset(tabId));
    for (const report of recent) {
      report.media.pageUrl = isDownloadAddress(url) ? url : undefined;
      this.connection.post(makeMedia(report.media));
    }
  }

  /** The tab loads another page, or closed: forget what it had. */
  reset(tabId) {
    if (this.tabs.delete(tabId)) {
      this.connection.post(makeMediaReset(tabId));
    }
  }

  /** A new LDM connected: it knows none of the earlier reports. */
  forgetAll() {
    this.tabs.clear();
  }

  /** @private */
  stateOf(tabId) {
    let state = this.tabs.get(tabId);
    if (!state) {
      state = { reported: new Set(), streamSeen: false, page: undefined, reports: [] };
      this.tabs.set(tabId, state);
    }
    return state;
  }
}

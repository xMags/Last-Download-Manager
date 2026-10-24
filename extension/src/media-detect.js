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

export class MediaDetector {
  /**
   * @param {object} api the extension API object
   * @param {{post: function(object): boolean}} connection the lasting connection to LDM
   * @param {{ready: Promise, isEnabled: function(): boolean, rules: function(): object, userAgent: string}} options
   */
  constructor(api, connection, options) {
    this.api = api;
    this.connection = connection;
    this.options = options;
    this.tabs = new Map();  // tab id -> {reported: Set of addresses, streamSeen: boolean}
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
    if (found.kind !== 'file') {
      state.streamSeen = true;
    }
    const storeId = await cookieStoreFor(this.api, tab);
    const cookies = storeId === null ? [] : await this.api.cookies.getAll({ url: details.url, ...(storeId ? { storeId } : {}) })
      .catch(() => []);
    this.connection.post(makeMedia({
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
    }));
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
      state = { reported: new Set(), streamSeen: false };
      this.tabs.set(tabId, state);
    }
    return state;
  }
}

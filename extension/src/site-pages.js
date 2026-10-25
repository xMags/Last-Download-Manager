import { isDownloadAddress, makeSitePage } from './protocol.js';

// Tells LDM the address of tabs on the video sites it supports (video-sites design, section 7),
// so it can look a video up when the page shows one, also when the site changes the video without
// loading a page. Only these sites' own addresses are sent, and only while LDM asked for them
// (config sitePages); LDM checks each address itself and builds its own from it.

// The sites' hosts. LDM decides which of their pages are videos.
const SITE_HOSTS = new Set(['youtube.com', 'www.youtube.com', 'm.youtube.com', 'youtu.be', 'youtube-nocookie.com',
  'www.youtube-nocookie.com']);

/** True for an address on one of the supported sites. */
export function isSiteAddress(url) {
  if (!isDownloadAddress(url) || !/^https?:/i.test(url)) {
    return false;
  }
  try {
    return SITE_HOSTS.has(new URL(url).hostname.toLowerCase());
  } catch {
    return false;
  }
}

export class SitePages {
  /**
   * @param {object} api the extension API object
   * @param {{post: function(object): boolean}} connection the lasting connection to LDM
   * @param {{ready: Promise, isEnabled: function(): boolean, userAgent: string}} options
   */
  constructor(api, connection, options) {
    this.api = api;
    this.connection = connection;
    this.options = options;
    this.sent = new Map();  // tab id -> the address last sent
  }

  /** tabs.onUpdated: the address changed, or a page finished loading at the same address. */
  async onUpdated(tabId, changeInfo, tab) {
    if (changeInfo.url === undefined && changeInfo.status !== 'complete') {
      return;
    }
    await this.options.ready;
    this.report(tabId, tab);
  }

  /** The tab loads another page, or closed: its next address is new to LDM. */
  reset(tabId) {
    this.sent.delete(tabId);
  }

  /** A new LDM connected, or site videos were turned on: the open tabs' addresses again. */
  async announceAll() {
    this.sent.clear();
    await this.options.ready;
    const tabs = await this.api.tabs.query({}).catch(() => []);
    for (const tab of tabs ?? []) {
      this.report(tab.id, tab);
    }
  }

  /** @private */
  report(tabId, tab) {
    if (!this.options.isEnabled() || !Number.isSafeInteger(tabId) || tabId < 0 || !tab) {
      return;
    }
    const url = tab.url;
    const previous = this.sent.get(tabId);
    // Leaving the site for another one loads a page, and LDM forgets the tab then.
    if (!isSiteAddress(url) || url === previous) {
      return;
    }
    const message = makeSitePage({
      tabId,
      incognito: Boolean(tab.incognito),
      url,
      pageTitle: tab.title,
      userAgent: this.options.userAgent,
    });
    if (message && this.connection.post(message)) {
      this.sent.set(tabId, url);
    }
  }
}

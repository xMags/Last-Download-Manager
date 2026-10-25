import { isDownloadAddress, makeSitePage } from './protocol.js';

// Tells LDM the address of tabs on the video sites it lists (video-sites designs, round 1
// section 7 and round 2 section 2.2), so it can look a video up when the page shows one, also when
// the site changes the video without loading a page. Only these sites' own addresses are sent,
// and only while LDM asked for them (config sitePages); LDM checks each address itself and builds
// its own from it.

// The sites of LDM 5.1, which sends no list of its own.
const DEFAULT_SITE_HOSTS = ['youtube.com', 'youtu.be', 'youtube-nocookie.com'];

/** True for an address on one of `hosts` or their subdomains. LDM decides which pages are videos. */
export function isSiteAddress(url, hosts = DEFAULT_SITE_HOSTS) {
  if (!isDownloadAddress(url) || !/^https?:/i.test(url)) {
    return false;
  }
  try {
    const hostname = new URL(url).hostname.toLowerCase();
    return hosts.some((host) => hostname === host || hostname.endsWith('.' + host));
  } catch {
    return false;
  }
}

export class SitePages {
  /**
   * @param {object} api the extension API object
   * @param {{post: function(object): boolean}} connection the lasting connection to LDM
   * @param {{ready: Promise, isEnabled: function(): boolean, hosts: function(): (string[]|null),
   *          signInHosts?: function(): string[], cookiesFor?: function(string, object): Promise<object[]>,
   *          userAgent: string}} options
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
    await this.report(tabId, tab);
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
    await Promise.all((tabs ?? []).map((tab) => this.report(tab.id, tab)));
  }

  /** @private */
  async report(tabId, tab) {
    if (!this.options.isEnabled() || !Number.isSafeInteger(tabId) || tabId < 0 || !tab) {
      return;
    }
    const url = tab.url;
    const previous = this.sent.get(tabId);
    // Leaving the site for another one loads a page, and LDM forgets the tab then.
    if (!isSiteAddress(url, this.options.hosts?.() ?? DEFAULT_SITE_HOSTS) || url === previous) {
      return;
    }
    // Taken before the cookies are read, so a second report of the same address waits for none.
    this.sent.set(tabId, url);
    // The user's sign-in, only for the sites LDM's Options let sign in.
    const signIn = isSiteAddress(url, this.options.signInHosts?.() ?? []) && this.options.cookiesFor;
    const cookies = signIn ? await this.options.cookiesFor(url, tab).catch(() => []) : undefined;
    const message = makeSitePage({
      tabId,
      incognito: Boolean(tab.incognito),
      url,
      pageTitle: tab.title,
      userAgent: this.options.userAgent,
      cookies,
    });
    if (!message || !this.connection.post(message)) {
      if (this.sent.get(tabId) === url) {
        this.sent.delete(tabId);
      }
    }
  }
}

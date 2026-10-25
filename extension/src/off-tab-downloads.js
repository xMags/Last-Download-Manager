// Tells whether a browser download came from a tab where the user turned integration off
// (src/off-tabs.js), so the capture leaves it to the browser. The downloads API does not say which
// tab started a download, so two signs are used:
//   1. The page request that turned into the download. A link that downloads loads in its tab or
//      frame first, and webRequest names that tab. Only requests of switched-off tabs are kept.
//   2. The download's referrer, the page it came from (also for downloads a page starts itself,
//      which make no request a tab owns). It counts only when every open tab showing that page is
//      switched off.
// Without either sign the download is captured as usual: a switched-on tab never loses a capture
// to a guess.

// A request that does not become a download within this time is not the start of one.
const LIFETIME_MS = 60000;
const MAX_REQUESTS = 50;

export class OffTabDownloads {
  /**
   * @param {object} api the extension API object
   * @param {function(number): boolean} isTabOff true for a tab where integration is off
   * @param {function(): number} [now] milliseconds
   */
  constructor(api, isTabOff, now = () => Date.now()) {
    this.api = api;
    this.isTabOff = isTabOff;
    this.now = now;
    this.requests = [];  // {url, tabId, at} of page and frame loads in switched-off tabs, oldest first
  }

  /** webRequest.onBeforeRequest for main_frame and sub_frame; redirects come here again. */
  onBeforeRequest(details) {
    if (!Number.isSafeInteger(details.tabId) || details.tabId < 0 || !this.isTabOff(details.tabId)) {
      return;
    }
    this.requests.push({ url: details.url, tabId: details.tabId, at: this.now() });
    if (this.requests.length > MAX_REQUESTS) {
      this.requests.shift();
    }
  }

  /**
   * True when the download came from a switched-off tab.
   * @param {{url: string, finalUrl?: string, referrer?: string}} download
   */
  async fromOffTab({ url, finalUrl, referrer }) {
    const now = this.now();
    this.requests = this.requests.filter((request) => now - request.at <= LIFETIME_MS);
    const request = this.requests.findLast((candidate) => candidate.url === url || candidate.url === finalUrl);
    if (request) {
      // A tab turned on again since then has its downloads back.
      return this.isTabOff(request.tabId);
    }
    return this.referrerTabsAreOff(referrer);
  }

  /** @private */
  async referrerTabsAreOff(referrer) {
    let page;
    try {
      page = new URL(referrer);
    } catch {
      return false;
    }
    if (page.protocol !== 'http:' && page.protocol !== 'https:') {
      return false;
    }
    const tabs = (await this.api.tabs.query({}).catch(() => [])) ?? [];
    let showing = tabs.filter((tab) => tab.url === referrer);
    // A cross-site download usually names only the site ("https://a.test/"), not the page.
    if (showing.length === 0 && page.pathname === '/' && !page.search) {
      showing = tabs.filter((tab) => originOf(tab.url) === page.origin);
    }
    return showing.length > 0 && showing.every((tab) => this.isTabOff(tab.id));
  }
}

function originOf(url) {
  try {
    return new URL(url).origin;
  } catch {
    return null;
  }
}

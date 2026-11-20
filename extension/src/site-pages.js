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
    this.sent = new Map();  // tab id -> what was last sent: the address and the post, one per line
    this.posts = new Map();  // tab id -> the link to the post whose video the tab plays
    this.watched = new Set();  // tab ids whose page script looks for that post
  }

  /**
   * The tab's page script found the post whose video plays (or none any more): a feed's video is
   * looked up as that post. Only the top frame's word counts; the link is the page's, so LDM checks it.
   */
  async onPostFound(tabId, frameId, postUrl) {
    if (frameId !== 0 || !this.watched.has(tabId)) {
      return;
    }
    const post = typeof postUrl === 'string' && isSiteAddress(postUrl, this.hosts()) ? postUrl : undefined;
    if (this.posts.get(tabId) === post) {
      return;
    }
    if (post === undefined) {
      this.posts.delete(tabId);
    } else {
      this.posts.set(tabId, post);
    }
    const tab = await this.api.tabs.get(tabId).catch(() => null);
    if (tab) {
      await this.report(tabId, tab);
    }
  }

  /** A frame's page script started: a watched tab's top frame looks for the post again. */
  frameReady(tabId, frameId) {
    if (frameId === 0 && this.watched.has(tabId)) {
      this.tellWatch(tabId, true);
    }
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
    this.posts.delete(tabId);
    this.watched.delete(tabId);  // the new page's script starts without watching
  }

  /** A new LDM connected, or site videos were turned on: the open tabs' addresses again. */
  async announceAll() {
    this.sent.clear();
    await this.options.ready;
    const tabs = await this.api.tabs.query({}).catch(() => []);
    await Promise.all((tabs ?? []).map((tab) => this.report(tab.id, tab)));
  }

  /** Site videos were turned off, or LDM left: the page scripts stop looking for posts. */
  stopWatching() {
    for (const tabId of this.watched) {
      this.tellWatch(tabId, false);
    }
    this.watched.clear();
    this.posts.clear();
  }

  /** @private The sites LDM lists. */
  hosts() {
    return this.options.hosts?.() ?? DEFAULT_SITE_HOSTS;
  }

  /** @private */
  tellWatch(tabId, on) {
    this.api.tabs.sendMessage?.(tabId, { type: 'ldm-watch-posts', on }, { frameId: 0 })?.catch?.(() => {});
  }

  /** @private */
  async report(tabId, tab) {
    if (!this.options.isEnabled() || !Number.isSafeInteger(tabId) || tabId < 0 || !tab) {
      return;
    }
    const url = tab.url;
    // Leaving the site for another one loads a page, and LDM forgets the tab then.
    if (!isSiteAddress(url, this.hosts())) {
      return;
    }
    // A page of the site may play posts (a feed); its script looks for the one that plays.
    if (!this.watched.has(tabId)) {
      this.watched.add(tabId);
      this.tellWatch(tabId, true);
    }
    const postUrl = this.posts.get(tabId);
    const key = `${url}
${postUrl ?? ''}`;
    if (key === this.sent.get(tabId)) {
      return;
    }
    // Taken before the cookies are read, so a second report of the same address waits for none.
    this.sent.set(tabId, key);
    // The user's sign-in, only for the sites LDM's Options let sign in.
    const signIn = isSiteAddress(url, this.options.signInHosts?.() ?? []) && this.options.cookiesFor;
    const cookies = signIn ? await this.options.cookiesFor(url, tab).catch(() => []) : undefined;
    const message = makeSitePage({
      tabId,
      incognito: Boolean(tab.incognito),
      url,
      postUrl,
      pageTitle: tab.title,
      userAgent: this.options.userAgent,
      cookies,
    });
    if (!message || !this.connection.post(message)) {
      if (this.sent.get(tabId) === key) {
        this.sent.delete(tabId);
      }
    }
  }
}

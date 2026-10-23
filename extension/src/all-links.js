// "Download all links with LDM" and "Download selected links with LDM" (checklist B-04, B-05):
// collects the page's links, sends them to LDM, which opens its Download All Links dialog, and
// then checks the links' types and sizes for that dialog until it closes.

import { cookieStoreFor } from './cookie-stores.js';
import { LinkChecker } from './link-check.js';
import { collectPageLinks } from './page-links.js';
import { isLinksDone, linksAccepted, makeLinkInfo, makeLinks, MAX_PAGE_LINKS } from './protocol.js';

// Long enough for ldm-host.exe to start LDM and for LDM to answer, as for captures.
const ANSWER_TIMEOUT_MS = 15000;
// Cookie lookups per page: one per distinct address path, at most this many.
const MAX_COOKIE_LOOKUPS = 2000;

export const LinksMessage = Object.freeze({
  NoLinks: 'Last Download Manager cannot find any links on this page.\n\nIf the page does have links, ' +
    'reload it and try again.',
  NoSelectedLinks: 'Last Download Manager cannot find any links in the selected part of the page.',
  NoContact: 'The browser cannot reach Last Download Manager.\n\nMake sure LDM is installed and has been ' +
    'started at least once, so it can register itself with this browser.',
});

export class AllLinks {
  /**
   * @param {object} api the extension API object
   * @param {import('./connection.js').LdmConnection} connection
   * @param {{userAgent: string, tell: function(object, string)}} options tell(tab, text) shows a
   *   message in the page the menu was used on
   */
  constructor(api, connection, options) {
    this.api = api;
    this.connection = connection;
    this.options = options;
  }

  /**
   * @param {object} tab where the menu was used
   * @param {number} frameId the frame the menu was used in (the selection lives there)
   * @param {boolean} selection only the selected links
   */
  async start(tab, frameId, selection) {
    const storeId = await cookieStoreFor(this.api, tab);
    const links = await this.collect(tab, frameId, selection);
    const noLinks = selection ? LinksMessage.NoSelectedLinks : LinksMessage.NoLinks;
    if (links.length === 0) {
      this.options.tell(tab, noLinks);
      return;
    }
    const message = makeLinks({
      selection,
      referrer: tab.url,
      userAgent: this.options.userAgent,
      links,
      cookies: storeId === null ? [] : await this.cookiesFor(links.map((link) => link.url), storeId),
    });
    if (!message) {
      this.options.tell(tab, noLinks);
      return;
    }

    let conversation = null;
    const checker = new LinkChecker(globalThis.fetch.bind(globalThis), (items) => conversation?.send(makeLinkInfo(items)));
    conversation =await this.connection.conversation(message, ANSWER_TIMEOUT_MS, {
      onMessage: (received) => {
        if (isLinksDone(received, message.id)) {
          conversation?.close();
        }
      },
      onEnd: () => checker.stop(),
    });
    if (!conversation) {
      this.options.tell(tab, LinksMessage.NoContact);
      return;
    }
    if (!linksAccepted(conversation.answer, message.id)) {
      conversation.close();
      this.options.tell(tab, noLinks);
      return;
    }
    await checker.run(message.links.map((link) => link.url));
    // Everything is checked: the dialog needs nothing more from this connection.
    conversation.close();
  }

  /** @private The links of the page (every frame) or of the selection (its frame), page order. */
  async collect(tab, frameId, selection) {
    let results;
    try {
      results = await this.api.scripting.executeScript({
        target: selection ? { tabId: tab.id, frameIds: [frameId] } : { tabId: tab.id, allFrames: true },
        func: collectPageLinks,
        args: [selection, MAX_PAGE_LINKS],
      });
    } catch (error) {
      console.warn('LDM: could not read the links of this page', error);
      return [];
    }
    // The top frame first, then the frames in the order the browser numbered them.
    const ordered = [...(results ?? [])].sort((a, b) => (a.frameId ?? 0) - (b.frameId ?? 0));
    return ordered.flatMap((entry) => (Array.isArray(entry.result) ? entry.result : []));
  }

  /** @private The cookies the browser would send to these addresses (ENG-14). */
  async cookiesFor(urls, storeId) {
    const paths = new Set();
    for (const url of urls) {
      try {
        const parsed = new URL(url);
        if (/^(https?|ftps?):$/.test(parsed.protocol)) {
          paths.add(`${parsed.origin}${parsed.pathname}`);
        }
      } catch {
        // not an address; makeLinks drops it
      }
      if (paths.size >= MAX_COOKIE_LOOKUPS) {
        break;
      }
    }
    const details = (url) => (storeId ? { url, storeId } : { url });
    const lists = await Promise.all([...paths].map((url) => this.api.cookies.getAll(details(url)).catch(() => [])));
    return lists.flat();
  }
}

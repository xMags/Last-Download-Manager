// Takes browser downloads over (checklist B-02, M3 design section 5). The browser's download is
// paused, not cancelled, until LDM has accepted it: if LDM cannot be reached or refuses, the
// browser simply continues, so a download is never lost.

import { baseName, isCapturableAddress, nameFromUrl, shouldCapture } from './capture-rules.js';
import { cookieStoreFor } from './cookie-stores.js';
import { isAcceptedCapture, makeCapture } from './protocol.js';

// Long enough for ldm-host.exe to start LDM (it waits up to 10 seconds) and for LDM to answer,
// which LDM does before showing any dialog.
const ANSWER_TIMEOUT_MS = 15000;

export class DownloadCapture {
  /**
   * @param {object} api the extension API object
   * @param {import('./connection.js').LdmConnection} connection
   * @param {{ready: Promise, isEnabled: function(): boolean, rules: function(): object,
   *          keyVerdict: function(object): string,
   *          userAgent: string, canHideDownloadUi: boolean}} options
   *   ready settles once the stored settings are loaded; a download can wake the extension first.
   *   keyVerdict gives 'prevent', 'force' or 'none' for {url, finalUrl, referrer, mimeType, fileName}.
   *   postFor (optional) gives the form submission a download answered, or null.
   */
  constructor(api, connection, options) {
    this.api = api;
    this.connection = connection;
    this.options = options;
    this.hiding = 0;  // captures in progress that keep the browser's download bubble hidden
  }

  /**
   * Decides about one browser download and takes it over when the rules say so.
   * @param {object} item the browser's DownloadItem
   * @param {string} fileName the name the browser chose, without folders
   * @returns {Promise<boolean>} true when LDM took the download over
   */
  async take(item, fileName) {
    await this.options.ready;
    if (!this.options.isEnabled() || item.state !== 'in_progress' || item.byExtensionId === this.api.runtime.id) {
      return false;
    }
    const url = item.url;
    const finalUrl = item.finalUrl || item.url;
    const verdict = this.options.keyVerdict({ url, finalUrl, referrer: item.referrer, mimeType: item.mime, fileName });
    if (verdict === 'prevent') {
      return false;
    }
    // Forced downloads skip the type list and the exceptions (checklist B-03).
    const wanted = verdict === 'force' ? isCapturableAddress(url) : shouldCapture({ url, finalUrl, fileName }, this.options.rules());
    if (!wanted) {
      return false;
    }
    // Private windows have their own cookies (checklist B-11); without them, leave it to the browser.
    const storeId = await cookieStoreFor(this.api, item);
    if (storeId === null) {
      return false;
    }

    this.hideDownloadUi();
    try {
      if (!(await this.pause(item.id))) {
        return false;  // finished or failed already: nothing left to take over
      }
      const message = makeCapture({
        url,
        finalUrl,
        referrer: item.referrer,
        fileName,
        fileSize: item.totalBytes > 0 ? item.totalBytes : item.fileSize,
        mimeType: item.mime,
        userAgent: this.options.userAgent,
        cookies: await this.cookiesFor([url, finalUrl], storeId),
        post: this.options.postFor?.(url) ?? undefined,
        chosen: verdict === 'force',
      });
      const answer = await this.connection.oneShot(message, ANSWER_TIMEOUT_MS);
      if (!isAcceptedCapture(answer, message.id)) {
        await this.resume(item.id);
        return false;
      }
      await this.api.downloads.cancel(item.id).catch(() => {});
      await this.api.downloads.erase({ id: item.id }).catch(() => {});
      return true;
    } finally {
      this.showDownloadUi();
    }
  }

  /**
   * Chrome and Edge: decides before the browser settles the download's name, then lets it go on.
   * A browser that asks where to save (Chrome always does in private windows) shows that dialog
   * once the name is settled, so it then asks only about downloads LDM left to it.
   * @param {object} item the DownloadItem of onDeterminingFilename
   * @param {function(): void} suggest that event's callback; called once for every download the
   *   browser keeps, also on errors. A download LDM took is erased already, and Chrome reports
   *   suggesting a name for it as an error.
   * @returns {Promise<boolean>} true when LDM took the download over
   */
  async takeBeforeNaming(item, suggest) {
    let taken = false;
    try {
      taken = await this.take(item, baseName(item.filename));
      return taken;
    } finally {
      if (!taken) {
        suggest();  // keep the browser's own name
      }
    }
  }

  /**
   * Gives a link clicked with the force keys straight to LDM (the browser does not load it).
   * `chosen`: the user picked LDM for this link in the right-click menu, which works also while
   * the toolbar button has integration turned off. `tab`: where the link is, for its cookies.
   * @returns {Promise<boolean>} true when LDM accepted it
   */
  async sendLink(href, pageUrl, { chosen = false, tab } = {}) {
    await this.options.ready;
    if ((!chosen && !this.options.isEnabled()) || !isCapturableAddress(href)) {
      return false;
    }
    const storeId = await cookieStoreFor(this.api, tab);
    if (storeId === null) {
      return false;
    }
    const message = makeCapture({
      url: href,
      referrer: pageUrl,
      fileName: nameFromUrl(href),
      userAgent: this.options.userAgent,
      cookies: await this.cookiesFor([href], storeId),
      chosen: true,  // the menu or the force keys: never an automatic capture
    });
    return isAcceptedCapture(await this.connection.oneShot(message, ANSWER_TIMEOUT_MS), message.id);
  }

  /** @private Pauses the download; false when it is no longer running. */
  async pause(id) {
    await this.api.downloads.pause(id).catch(() => {});
    const [current] = await this.api.downloads.search({ id });
    return current?.state === 'in_progress';
  }

  /** @private */
  async resume(id) {
    await this.api.downloads.resume(id).catch(() => {});
  }

  /** @private The cookies the browser would send to these addresses (ENG-14), from `storeId` or the normal store. */
  async cookiesFor(urls, storeId) {
    const details = (url) => (storeId ? { url, storeId } : { url });
    const lists = await Promise.all([...new Set(urls)].map((url) => this.api.cookies.getAll(details(url)).catch(() => [])));
    return lists.flat();
  }

  /** @private Chrome and Edge only: keeps the download bubble from flashing for captured files. */
  hideDownloadUi() {
    this.hiding += 1;
    if (this.hiding === 1 && this.options.canHideDownloadUi) {
      this.api.downloads.setUiOptions({ enabled: false }).catch(() => {});
    }
  }

  /** @private */
  showDownloadUi() {
    this.hiding -= 1;
    if (this.hiding === 0 && this.options.canHideDownloadUi) {
      this.api.downloads.setUiOptions({ enabled: true }).catch(() => {});
    }
  }
}

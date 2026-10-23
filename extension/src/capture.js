// Takes browser downloads over (checklist B-02, M3 design section 5). The browser's download is
// paused, not cancelled, until LDM has accepted it: if LDM cannot be reached or refuses, the
// browser simply continues, so a download is never lost.

import { shouldCapture } from './capture-rules.js';
import { isAcceptedCapture, makeCapture } from './protocol.js';

// Long enough for ldm-host.exe to start LDM (it waits up to 10 seconds) and for LDM to answer,
// which LDM does before showing any dialog.
const ANSWER_TIMEOUT_MS = 15000;

export class DownloadCapture {
  /**
   * @param {object} api the extension API object
   * @param {import('./connection.js').LdmConnection} connection
   * @param {{ready: Promise, isEnabled: function(): boolean, patterns: function(): string[],
   *          userAgent: string, canHideDownloadUi: boolean}} options
   *   ready settles once the stored settings are loaded; a download can wake the extension first.
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
    // Private windows come with their own cookie store and need the user's permission: handled
    // separately (checklist B-11).
    if (item.incognito) {
      return false;
    }
    const url = item.url;
    const finalUrl = item.finalUrl || item.url;
    if (!shouldCapture({ url: finalUrl, fileName }, this.options.patterns())) {
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
        cookies: await this.cookiesFor([url, finalUrl]),
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

  /** @private The cookies the browser would send to these addresses (ENG-14). */
  async cookiesFor(urls) {
    const lists = await Promise.all([...new Set(urls)].map((url) => this.api.cookies.getAll({ url }).catch(() => [])));
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

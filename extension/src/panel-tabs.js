// The tabs where the user turned the video panel off with the toolbar button. A tab stays off
// across page loads until the button is clicked again or the tab closes. Kept in storage.session,
// so a background script stopped while idle still knows them, and a browser restart forgets them.

const STORAGE_KEY = 'panelTabs';

export class PanelTabs {
  /** @param {object} api the extension API object */
  constructor(api) {
    this.api = api;
    // tab id -> true when the panel is off. A tab whose button was ever clicked stays listed (with
    // false once on again): the browser keeps that tab's own button look until the tab closes.
    this.tabs = new Map();
    this.ready = this.load();
  }

  /** @private */
  async load() {
    const stored = await this.api.storage.session?.get(STORAGE_KEY).catch(() => null);
    const entries = stored?.[STORAGE_KEY];
    if (!Array.isArray(entries)) {
      return;
    }
    for (const entry of entries) {
      if (Array.isArray(entry) && Number.isSafeInteger(entry[0]) && typeof entry[1] === 'boolean') {
        this.tabs.set(entry[0], entry[1]);
      }
    }
  }

  /** @private */
  save() {
    return this.api.storage.session?.set({ [STORAGE_KEY]: [...this.tabs] }).catch(() => {});
  }

  /** True when the user turned the panel off in this tab. */
  isOff(tabId) {
    return this.tabs.get(tabId) === true;
  }

  /** The tabs with a button look of their own. */
  switchedTabs() {
    return [...this.tabs.keys()];
  }

  /** Turns the panel off in the tab, or on again; returns true when it is now off. */
  async toggle(tabId) {
    await this.ready;
    const off = !this.isOff(tabId);
    this.tabs.set(tabId, off);
    await this.save();
    return off;
  }

  /** The tab closed. */
  async forget(tabId) {
    await this.ready;
    if (this.tabs.delete(tabId)) {
      await this.save();
    }
  }
}

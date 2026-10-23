// Remembers recent link clicks and the keys held in pages, as reported by src/page-keys.js, so a
// download can be matched to the click that started it.

import { NO_KEYS } from './key-rules.js';

// A click that does not start its download within this time is no longer related to it.
const CLICK_LIFETIME_MS = 15000;
// A download that names the clicked page as its referrer counts as started by the click within this time.
const REFERRER_MATCH_MS = 5000;
// Key state not refreshed for this long is treated as released (a page can miss its key-up).
const HELD_LIFETIME_MS = 60000;
const MAX_CLICKS = 20;

export class LinkClicks {
  constructor(now = () => Date.now()) {
    this.now = now;
    this.clicks = [];
    this.held = { keys: NO_KEYS, at: 0 };
  }

  recordClick({ href, pageUrl, keys }) {
    this.clicks.push({ href, pageUrl, keys, at: this.now() });
    if (this.clicks.length > MAX_CLICKS) {
      this.clicks.shift();
    }
  }

  recordKeys(keys) {
    this.held = { keys, at: this.now() };
  }

  /** Keys down in the page right now. */
  heldKeys() {
    return this.now() - this.held.at <= HELD_LIFETIME_MS ? this.held.keys : NO_KEYS;
  }

  /**
   * The keys of the click that started this download: the newest recent click on its address, or
   * a very recent click on the page it came from. Null when no click matches.
   */
  clickKeysFor({ url, finalUrl, referrer }) {
    const now = this.now();
    this.clicks = this.clicks.filter((click) => now - click.at <= CLICK_LIFETIME_MS);
    for (let i = this.clicks.length - 1; i >= 0; i -= 1) {
      const click = this.clicks[i];
      const sameLink = click.href === url || click.href === finalUrl;
      const fromPage = referrer && click.pageUrl === referrer && now - click.at <= REFERRER_MATCH_MS;
      if (sameLink || fromPage) {
        return click.keys;
      }
    }
    return null;
  }
}

/** Key states from a page are untrusted: only these five booleans are kept. */
export function cleanKeys(keys) {
  const bool = (value) => value === true;
  return {
    alt: bool(keys?.alt),
    shift: bool(keys?.shift),
    ctrl: bool(keys?.ctrl),
    insert: bool(keys?.insert),
    delete: bool(keys?.delete),
  };
}

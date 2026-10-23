// What the special keys mean for one download (checklist B-03). Pure functions; the key state
// comes from the page script (src/page-keys.js) through LinkClicks.

export const NO_KEYS = Object.freeze({ alt: false, shift: false, ctrl: false, insert: false, delete: false });

const PREVENT_KEYS = ['alt', 'shift', 'ctrl'];
const FORCE_KEYS = ['alt', 'shift', 'ctrl', 'insert', 'delete'];

/** The key settings from LDM's config, with LDM's defaults for anything missing or malformed. */
export function keySettings(config) {
  const flag = (value, fallback) => (typeof value === 'boolean' ? value : fallback);
  const keys = config?.keys ?? {};
  return {
    prevent: {
      enabled: flag(keys.prevent?.enabled, true),
      alt: flag(keys.prevent?.alt, true),
      shift: flag(keys.prevent?.shift, false),
      ctrl: flag(keys.prevent?.ctrl, false),
    },
    force: {
      enabled: flag(keys.force?.enabled, true),
      alt: flag(keys.force?.alt, false),
      shift: flag(keys.force?.shift, false),
      ctrl: flag(keys.force?.ctrl, false),
      insert: flag(keys.force?.insert, true),
      delete: flag(keys.force?.delete, false),
    },
    forceNeedsLinkClick: flag(keys.forceNeedsLinkClick, true),
    skipWebContent: flag(keys.skipWebContent, true),
  };
}

/** True when the set is on, has at least one key, and every key of it is down. */
function setHeld(set, names, keys) {
  if (!set.enabled || !keys) {
    return false;
  }
  const chosen = names.filter((name) => set[name]);
  return chosen.length > 0 && chosen.every((name) => keys[name] === true);
}

export function preventHeld(settings, keys) {
  return setHeld(settings.prevent, PREVENT_KEYS, keys);
}

export function forceHeld(settings, keys) {
  return setHeld(settings.force, FORCE_KEYS, keys);
}

/**
 * The keys' verdict for a download.
 * @param settings from keySettings
 * @param {object|null} clickKeys keys held while the download's link was clicked, if it was
 * @param {object} heldKeys keys down in the page right now
 * @returns {'prevent'|'force'|'none'}
 */
export function keyVerdict(settings, clickKeys, heldKeys) {
  if (preventHeld(settings, clickKeys) || preventHeld(settings, heldKeys)) {
    return 'prevent';
  }
  const forced = settings.forceNeedsLinkClick ? forceHeld(settings, clickKeys)
    : forceHeld(settings, clickKeys) || forceHeld(settings, heldKeys);
  return forced ? 'force' : 'none';
}

/**
 * The keys' verdict for one browser download: like keyVerdict, but forcing leaves web pages,
 * pictures and scripts to the browser when the settings ask for that.
 */
export function downloadVerdict(settings, clickKeys, heldKeys, mimeType, fileName) {
  const verdict = keyVerdict(settings, clickKeys, heldKeys);
  if (verdict === 'force' && settings.skipWebContent && isWebContent(mimeType, fileName)) {
    return 'none';
  }
  return verdict;
}

const WEB_TYPES = /^(text\/(html|css|javascript|plain|xml)|application\/(xhtml\+xml|javascript|json|xml)|image\/)/i;
const WEB_EXTENSIONS = /\.(html?|xhtml|php\d?|aspx?|jsp|cgi|js|mjs|css|json|xml|txt|jpe?g|png|gif|webp|avif|bmp|ico|svg)$/i;

/** True for web pages, pictures and scripts, which forcing leaves to the browser when asked to. */
export function isWebContent(mimeType, fileName) {
  return WEB_TYPES.test(mimeType ?? '') || WEB_EXTENSIONS.test(fileName ?? '');
}

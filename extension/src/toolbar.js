// The toolbar button's extension: no popup; a click turns integration on or off, and
// the icon, badge and tooltip show on, off and error.

import { State } from './connection.js';

const TITLE = 'Last Download Manager';
const TOGGLE_HINT = '(click to turn browser integration on or off)';

const ICONS = {
  on: { 16: 'icons/ldm-16.png', 32: 'icons/ldm-32.png' },
  off: { 16: 'icons/ldm-off-16.png', 32: 'icons/ldm-off-32.png' },
};

/**
 * What the button looks like.
 * @param {{enabled: boolean, connection: string}} status
 * @returns {{icon: 'on'|'off', badge: string, title: string}}
 */
export function toolbarAppearance({ enabled, connection }) {
  if (!enabled) {
    return { icon: 'off', badge: '', title: `${TITLE} [disabled]\n${TOGGLE_HINT}` };
  }
  if (connection === State.Error) {
    return {
      icon: 'on',
      badge: '!',
      title: `${TITLE} [error]\nThe browser cannot reach LDM. Start Last Download Manager once so ` +
             `it can register itself with this browser.\n${TOGGLE_HINT}`,
    };
  }
  if (connection === State.NotRunning) {
    return {
      icon: 'on',
      badge: '',
      title: `${TITLE}\nLDM is not running; it starts when a download is captured.\n${TOGGLE_HINT}`,
    };
  }
  return { icon: 'on', badge: '', title: `${TITLE}\n${TOGGLE_HINT}` };
}

/** Applies an appearance to the browser's action button. */
export async function applyToolbar(api, appearance) {
  await Promise.all([
    api.action.setIcon({ path: ICONS[appearance.icon] }),
    api.action.setBadgeText({ text: appearance.badge }),
    api.action.setBadgeBackgroundColor({ color: '#c42b1c' }),
    api.action.setTitle({ title: appearance.title }),
  ]);
}

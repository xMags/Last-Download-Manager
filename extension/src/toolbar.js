// The toolbar button: no popup. A click hides LDM's video panel in that tab or shows it again; the
// right-click menu turns integration on or off for the whole browser. The icon, badge and tooltip
// show on, off and error, for the browser and per tab.

import { State } from './connection.js';

const TITLE = 'Last Download Manager';
const PANEL_HINT = '(click to hide the video panel in this tab; right-click to turn integration off)';
const PANEL_OFF_HINT = '(click to show the video panel in this tab again)';
const ENABLE_HINT = '(click to turn browser integration on)';

// Absolute paths: Chrome resolves relative ones against the background script's folder (src/).
export const ICONS = {
  on: { 16: '/icons/ldm-16.png', 32: '/icons/ldm-32.png' },
  off: { 16: '/icons/ldm-off-16.png', 32: '/icons/ldm-off-32.png' },
};

/**
 * What the button looks like.
 * @param {{enabled: boolean, connection: string, panelOff?: boolean}} status
 *   panelOff: the user turned the video panel off in the tab this look is for
 * @returns {{icon: 'on'|'off', badge: string, title: string}}
 */
export function toolbarAppearance({ enabled, connection, panelOff = false }) {
  if (!enabled) {
    return { icon: 'off', badge: '', title: `${TITLE} [disabled]\n${ENABLE_HINT}` };
  }
  const look = connectionAppearance(connection);
  if (!panelOff) {
    return { ...look, title: `${look.title}\n${PANEL_HINT}` };
  }
  return { icon: 'off', badge: look.badge, title: `${look.title}\nThe video panel is off in this tab.\n${PANEL_OFF_HINT}` };
}

function connectionAppearance(connection) {
  if (connection === State.Error) {
    return {
      icon: 'on',
      badge: '!',
      title: `${TITLE} [error]\nThe browser cannot reach LDM. Start Last Download Manager once so ` +
             'it can register itself with this browser.',
    };
  }
  if (connection === State.Elevated) {
    return {
      icon: 'on',
      badge: '!',
      title: `${TITLE} [error]\nLDM runs as administrator, so the browser cannot reach it. Exit LDM and ` +
             'start it normally.',
    };
  }
  if (connection === State.NotRunning) {
    return { icon: 'on', badge: '', title: `${TITLE}\nLDM is not running; it starts when a download is captured.` };
  }
  return { icon: 'on', badge: '', title: TITLE };
}

/**
 * Applies an appearance to the browser's action button: for every tab without a look of its own,
 * or, with `tabId`, for that tab only (a tab's own look wins until the tab closes).
 */
export async function applyToolbar(api, appearance, tabId) {
  const where = Number.isSafeInteger(tabId) ? { tabId } : {};
  await Promise.all([
    api.action.setIcon({ ...where, path: ICONS[appearance.icon] }),
    api.action.setBadgeText({ ...where, text: appearance.badge }),
    api.action.setBadgeBackgroundColor({ ...where, color: '#c42b1c' }),
    api.action.setTitle({ ...where, title: appearance.title }),
  ]);
}

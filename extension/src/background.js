// The extension's background script: keeps the connection to LDM, the cached capture rules and the
// toolbar button. Runs as a service worker in Chrome and Edge and as an event page in Firefox, so
// it may be stopped when idle; everything that must survive lives in storage.local.

import { AllLinks, LinksMessage } from './all-links.js';
import { browserInfo } from './browser-info.js';
import { DownloadCapture } from './capture.js';
import { baseName } from './capture-rules.js';
import { LdmConnection, State } from './connection.js';
import { applyMenu, clickedAddress, menuItems, MenuId } from './context-menu.js';
import { FormPosts } from './form-posts.js';
import { downloadVerdict, keySettings, sendsForcedLinkToLdm } from './key-rules.js';
import { cleanKeys, LinkClicks } from './link-clicks.js';
import { isDownloadAddress, menuSettings } from './protocol.js';
import { applyToolbar, toolbarAppearance } from './toolbar.js';

const api = globalThis.browser ?? globalThis.chrome;

const RECONNECT_ALARM = 'ldm-reconnect';
// Chrome's shortest alarm period. Each attempt starts ldm-host.exe, which exits at once while LDM
// is not running, so this costs nothing noticeable.
const RECONNECT_MINUTES = 0.5;

let enabled = true;
// Capture rules from LDM's Options, kept while LDM is not running.
let rules = { capturedExtensions: [], siteExceptions: [], addressExceptions: [] };

const connection = new LdmConnection(api, {
  helloInfo: async () => ({
    ...(await browserInfo(api, navigator)),
    extensionVersion: api.runtime.getManifest().version,
    incognitoAllowed: await api.extension.isAllowedIncognitoAccess(),
  }),
  onConfig: (config) => {
    const menuChanged = JSON.stringify(menuSettings(config)) !== JSON.stringify(menuSettings(rules));
    rules = config;
    api.storage.local.set({ config });
    if (menuChanged) {
      updateMenu();
    }
  },
  onMessage: (message) => {
    // Refresh download address (checklist ENG-08): LDM asks to open the page a download came from.
    if (message?.type === 'open-page' && isDownloadAddress(message.url) && /^https?:/i.test(message.url)) {
      api.tabs.create({ url: message.url, active: true })
        .then((tab) => api.windows?.update(tab.windowId, { focused: true }))
        .catch((error) => console.warn('LDM: could not open the page', error));
    }
  },
  onState: (state) => {
    render();
    if (state === State.Connected || state === State.Connecting) {
      api.alarms.clear(RECONNECT_ALARM);
    } else {
      api.alarms.create(RECONNECT_ALARM, { periodInMinutes: RECONNECT_MINUTES });
    }
  },
});

const settingsLoaded = api.storage.local.get(['enabled', 'config']).then((stored) => {
  enabled = stored.enabled !== false;
  rules = { ...rules, ...(stored.config ?? {}) };
});

const clicks = new LinkClicks();
const forms = new FormPosts();

const capture = new DownloadCapture(api, connection, {
  ready: settingsLoaded,
  isEnabled: () => enabled,
  rules: () => rules,
  postFor: (url) => forms.postFor(url),
  keyVerdict: ({ url, finalUrl, referrer, mimeType, fileName }) => {
    const clickKeys = clicks.clickKeysFor({ url, finalUrl, referrer });
    const heldKeys = clicks.heldKeys();
    const verdict = downloadVerdict(keySettings(rules), clickKeys, heldKeys, mimeType, fileName);
    // Shown in the extension's own console only; helps answer "why did LDM (not) take this?".
    console.info('LDM: download', url, { verdict, clickKeys, heldKeys });
    return verdict;
  },
  userAgent: navigator.userAgent,
  canHideDownloadUi: typeof api.downloads.setUiOptions === 'function',
});

const allLinks = new AllLinks(api, connection, { userAgent: navigator.userAgent, tell });

/** Shows `text` in the page the user is on; pages the browser protects show nothing. */
function tell(tab, text) {
  if (tab?.id === undefined) {
    return;
  }
  api.scripting.executeScript({ target: { tabId: tab.id }, func: (message) => alert(message), args: [text] })
    .catch(() => console.warn('LDM:', text));
}

// Menu updates run one after another, so a removeAll never races the creates of another update.
let menuUpdate = Promise.resolve();
function updateMenu() {
  menuUpdate = menuUpdate
    .then(() => applyMenu(api, menuItems(menuSettings(rules))))
    .catch((error) => console.warn('LDM: could not update the right-click menu', error));
}

async function onMenuClick(info, tab) {
  await settingsLoaded;
  switch (info.menuItemId) {
    case MenuId.Link:
    case MenuId.Page: {
      const href = clickedAddress(info);
      if (!isDownloadAddress(href)) {
        tell(tab, 'Last Download Manager cannot download this address: it is only valid inside this page.');
      } else if (!(await capture.sendLink(href, isDownloadAddress(info.pageUrl) ? info.pageUrl : undefined,
        { chosen: true, tab }))) {
        tell(tab, LinksMessage.NoContact);
      }
      break;
    }
    case MenuId.Selected:
    case MenuId.All:
      await allLinks.start(tab, info.frameId ?? 0, info.menuItemId === MenuId.Selected);
      break;
    default:
      break;
  }
}

function render() {
  applyToolbar(api, toolbarAppearance({ enabled, connection: connection.state })).catch((error) => {
    console.warn('LDM: could not update the toolbar button', error);
  });
}

// Listeners are registered synchronously, so events that woke the script are not missed.
api.action.onClicked.addListener(async () => {
  await settingsLoaded;  // a click can wake the script before the stored state is read
  enabled = !enabled;
  await api.storage.local.set({ enabled });
  render();
});

if (api.downloads.onDeterminingFilename) {
  // Chrome and Edge: here the name is known, also when it comes from Content-Disposition.
  api.downloads.onDeterminingFilename.addListener((item, suggest) => {
    suggest();  // keep the browser's own name; the decision runs on its own
    capture.take(item, baseName(item.filename)).catch((error) => console.warn('LDM: capture failed', error));
  });
} else {
  // Firefox decides the name before the download is created.
  api.downloads.onCreated.addListener((item) => {
    capture.take(item, baseName(item.filename)).catch((error) => console.warn('LDM: capture failed', error));
  });
}

// Form submissions of pages and frames, for downloads that answer them.
const FORM_FILTER = { urls: ['<all_urls>'], types: ['main_frame', 'sub_frame'] };
api.webRequest.onBeforeRequest.addListener((details) => forms.onBeforeRequest(details), FORM_FILTER, ['requestBody']);
api.webRequest.onSendHeaders.addListener((details) => forms.onSendHeaders(details), FORM_FILTER, ['requestHeaders']);

api.contextMenus.onClicked.addListener((info, tab) => {
  onMenuClick(info, tab).catch((error) => console.warn('LDM: menu action failed', error));
});

api.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === RECONNECT_ALARM) {
    connection.connect();
  }
});

// Pages opened before the extension was installed or updated have no page script; give them one,
// so the special keys work there without reloading every tab.
async function addPageScriptToOpenTabs() {
  const tabs = await api.tabs.query({ url: ['http://*/*', 'https://*/*'] });
  await Promise.all(tabs.map((tab) => api.scripting.executeScript({
    target: { tabId: tab.id, allFrames: true },
    files: ['src/page-keys.js'],
  }).catch(() => {})));  // pages the browser protects refuse; nothing to do there
}

api.runtime.onInstalled.addListener((details) => {
  if (details.reason === 'install') {
    api.tabs.create({ url: api.runtime.getURL('pages/welcome.html') });
  }
  addPageScriptToOpenTabs().catch((error) => console.warn('LDM: could not prepare open tabs', error));
});

// Reports from src/page-keys.js in web pages. Page code runs beside that script, so every field
// is checked here. Returns true when `reply` will be called later.
function onPageMessage(message, sender, reply) {
  const pageUrl = typeof message.pageUrl === 'string' ? message.pageUrl : undefined;
  switch (message.type) {
    case 'keys':
      clicks.recordKeys(cleanKeys(message.keys));
      break;
    case 'link-click':
      if (isDownloadAddress(message.href)) {
        clicks.recordClick({ href: message.href, pageUrl, keys: cleanKeys(message.keys) });
      }
      break;
    case 'force-link': {
      // A link clicked with the force keys. Links that load in the browser instead (web pages and
      // links without a file extension, unless the settings say otherwise) are answered at once;
      // their download, if any, is forced when it starts.
      const settings = keySettings(rules);
      if (!enabled || !settings.force.enabled || !isDownloadAddress(message.href) || !sendsForcedLinkToLdm(settings, message.href)) {
        reply({ handled: false });
        return false;
      }
      capture.sendLink(message.href, isDownloadAddress(pageUrl) ? pageUrl : undefined, { tab: sender.tab })
        .then((accepted) => reply({ handled: accepted }))
        .catch(() => reply({ handled: false }));
      return true;
    }
    default:
      break;
  }
}

api.runtime.onMessage.addListener((message, sender, reply) => {
  if (sender.id !== api.runtime.id || message === null || typeof message !== 'object') {
    return false;
  }
  if (sender.tab) {
    return onPageMessage(message, sender, reply) === true;
  }
  // Status for the extension's own pages.
  if (!String(sender.url ?? '').startsWith(api.runtime.getURL(''))) {
    return false;
  }
  if (message?.type === 'status') {
    api.storage.local.get('config').then(({ config }) => {
      reply({ enabled, connection: connection.state, appVersion: config?.appVersion ?? '' });
    });
    return true;  // answers asynchronously
  }
  return false;
});

settingsLoaded.then(() => {
  render();
  updateMenu();
  connection.connect();
});

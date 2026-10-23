// The extension's background script: keeps the connection to LDM, the cached capture rules and the
// toolbar button. Runs as a service worker in Chrome and Edge and as an event page in Firefox, so
// it may be stopped when idle; everything that must survive lives in storage.local.

import { browserInfo } from './browser-info.js';
import { DownloadCapture } from './capture.js';
import { baseName } from './capture-rules.js';
import { LdmConnection, State } from './connection.js';
import { FormPosts } from './form-posts.js';
import { downloadVerdict, keySettings } from './key-rules.js';
import { cleanKeys, LinkClicks } from './link-clicks.js';
import { isDownloadAddress } from './protocol.js';
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
    rules = config;
    api.storage.local.set({ config });
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
  keyVerdict: ({ url, finalUrl, referrer, mimeType, fileName }) => downloadVerdict(
    keySettings(rules), clicks.clickKeysFor({ url, finalUrl, referrer }), clicks.heldKeys(), mimeType, fileName),
  userAgent: navigator.userAgent,
  canHideDownloadUi: typeof api.downloads.setUiOptions === 'function',
});

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

api.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === RECONNECT_ALARM) {
    connection.connect();
  }
});

api.runtime.onInstalled.addListener((details) => {
  if (details.reason === 'install') {
    api.tabs.create({ url: api.runtime.getURL('pages/welcome.html') });
  }
});

// Reports from src/page-keys.js in web pages. Page code runs beside that script, so every field
// is checked here.
function onPageMessage(message, sender) {
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
    case 'force-link':
      // Only from a top-level user click, and only when the settings ask for it.
      if (isDownloadAddress(message.href) && sender.tab && keySettings(rules).skipWebContent === false) {
        capture.sendLink(message.href, isDownloadAddress(pageUrl) ? pageUrl : undefined)
          .catch((error) => console.warn('LDM: link hand-over failed', error));
      }
      break;
    default:
      break;
  }
}

api.runtime.onMessage.addListener((message, sender, reply) => {
  if (sender.id !== api.runtime.id || message === null || typeof message !== 'object') {
    return false;
  }
  if (sender.tab) {
    onPageMessage(message, sender);
    return false;
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
  connection.connect();
});

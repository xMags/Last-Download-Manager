// The extension's background script: keeps the connection to LDM, the cached capture rules and the
// toolbar button. Runs as a service worker in Chrome and Edge and as an event page in Firefox, so
// it may be stopped when idle; everything that must survive lives in storage.local.

import { browserInfo } from './browser-info.js';
import { DownloadCapture } from './capture.js';
import { baseName } from './capture-rules.js';
import { LdmConnection, State } from './connection.js';
import { applyToolbar, toolbarAppearance } from './toolbar.js';

const api = globalThis.browser ?? globalThis.chrome;

const RECONNECT_ALARM = 'ldm-reconnect';
// Chrome's shortest alarm period. Each attempt starts ldm-host.exe, which exits at once while LDM
// is not running, so this costs nothing noticeable.
const RECONNECT_MINUTES = 0.5;

let enabled = true;
let patterns = [];  // captured extensions from LDM's Options, kept while LDM is not running

const connection = new LdmConnection(api, {
  helloInfo: async () => ({
    ...(await browserInfo(api, navigator)),
    extensionVersion: api.runtime.getManifest().version,
    incognitoAllowed: await api.extension.isAllowedIncognitoAccess(),
  }),
  onConfig: (config) => {
    patterns = config.capturedExtensions;
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
  patterns = stored.config?.capturedExtensions ?? [];
});

const capture = new DownloadCapture(api, connection, {
  ready: settingsLoaded,
  isEnabled: () => enabled,
  patterns: () => patterns,
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

// Status for the extension's own pages; nothing else may ask.
api.runtime.onMessage.addListener((message, sender, reply) => {
  if (sender.id !== api.runtime.id || !String(sender.url ?? '').startsWith(api.runtime.getURL(''))) {
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

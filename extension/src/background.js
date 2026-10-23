// The extension's background script: keeps the connection to LDM, the cached capture rules and the
// toolbar button. Runs as a service worker in Chrome and Edge and as an event page in Firefox, so
// it may be stopped when idle; everything that must survive lives in storage.local.

import { browserInfo } from './browser-info.js';
import { LdmConnection, State } from './connection.js';
import { applyToolbar, toolbarAppearance } from './toolbar.js';

const api = globalThis.browser ?? globalThis.chrome;

const RECONNECT_ALARM = 'ldm-reconnect';
// Chrome's shortest alarm period. Each attempt starts ldm-host.exe, which exits at once while LDM
// is not running, so this costs nothing noticeable.
const RECONNECT_MINUTES = 0.5;

let enabled = true;

const connection = new LdmConnection(api, {
  helloInfo: async () => ({
    ...(await browserInfo(api, navigator)),
    extensionVersion: api.runtime.getManifest().version,
    incognitoAllowed: await api.extension.isAllowedIncognitoAccess(),
  }),
  onConfig: (config) => {
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

function render() {
  applyToolbar(api, toolbarAppearance({ enabled, connection: connection.state })).catch(() => {});
}

// Listeners are registered synchronously, so events that woke the script are not missed.
api.action.onClicked.addListener(async () => {
  enabled = !enabled;
  await api.storage.local.set({ enabled });
  render();
});

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

api.storage.local.get('enabled').then((stored) => {
  enabled = stored.enabled !== false;
  render();
  connection.connect();
});

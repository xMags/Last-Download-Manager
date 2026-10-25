const api = globalThis.browser ?? globalThis.chrome;

const MESSAGES = {
  connected: (version) => `Connected to Last Download Manager ${version}.`,
  connecting: () => 'Connecting to Last Download Manager...',
  'not-running': () => 'Last Download Manager is not running. It starts by itself when a download is captured.',
  error: () => 'The browser cannot reach Last Download Manager. Start Last Download Manager once so it can ' +
               'register itself with this browser, then reopen this page.',
  elevated: () => 'Last Download Manager runs as administrator, so the browser cannot reach it. Exit it and ' +
                  'start it normally.',
};

async function showStatus() {
  const status = await api.runtime.sendMessage({ type: 'status' });
  const describe = MESSAGES[status?.connection] ?? MESSAGES.error;
  let text = describe(status?.appVersion ?? '');
  if (status && !status.enabled) {
    text += ' Browser integration is turned off; click the LDM button in the toolbar to turn it on.';
  }
  document.getElementById('status').textContent = text;
}

showStatus();
// The connection may still be starting when the page opens.
setInterval(showStatus, 2000);

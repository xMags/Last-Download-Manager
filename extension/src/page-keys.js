// Runs in every web page (checklist B-03): reports the special keys held and the links clicked to
// the extension, and hands a link clicked with the force keys to LDM instead of letting it load.
// A classic script, as content scripts cannot be modules, so forceHeld and sendsToLdm repeat
// key-rules.js in a few lines (extension/test/page-keys.test.mjs checks both give the same answers).
// Everything it sends is checked again in the background, because page code runs beside it.
(() => {
  if (globalThis.__ldmPageKeys) {
    return;
  }
  globalThis.__ldmPageKeys = true;
  const api = globalThis.browser ?? globalThis.chrome;
  const keys = { alt: false, shift: false, ctrl: false, insert: false, delete: false };
  let settings = null;  // the "keys" part of LDM's config
  let enabled = true;   // the toolbar button's state

  const send = (message) => {
    try {
      api.runtime.sendMessage(message).catch(() => {});
    } catch {
      // the extension was reloaded; this page's script is orphaned
    }
  };

  api.storage.local.get(['config', 'enabled']).then((stored) => {
    settings = stored.config?.keys ?? null;
    enabled = stored.enabled !== false;
  }).catch(() => {});
  api.storage.onChanged.addListener((changes, area) => {
    if (area === 'local' && changes.config) {
      settings = changes.config.newValue?.keys ?? null;
    }
    if (area === 'local' && changes.enabled) {
      enabled = changes.enabled.newValue !== false;
    }
  });

  const report = () => send({ type: 'keys', keys: { ...keys } });

  // On keyboard layouts with AltGr (English India, German, ...) the right Alt key is AltGr, which
  // Firefox reports with altKey false. People press it as Alt, so it counts as Alt.
  const altHeld = (event) => event.altKey || event.getModifierState?.('AltGraph') === true;

  const update = (event, down) => {
    const before = JSON.stringify(keys);
    keys.alt = altHeld(event);
    keys.shift = event.shiftKey;
    keys.ctrl = event.ctrlKey;
    if (event.key === 'Insert') {
      keys.insert = down;
    } else if (event.key === 'Delete') {
      keys.delete = down;
    }
    if (JSON.stringify(keys) !== before) {
      report();
    }
  };
  // Only the user's own input counts: page code can dispatch made-up key and mouse events, which
  // must not force, push or block a download.
  addEventListener('keydown', (event) => event.isTrusted && update(event, true), true);
  addEventListener('keyup', (event) => event.isTrusted && update(event, false), true);
  addEventListener('blur', () => {
    if (Object.values(keys).some(Boolean)) {
      Object.keys(keys).forEach((name) => { keys[name] = false; });
      report();
    }
  }, true);

  const linkOf = (event) => {
    for (const node of event.composedPath()) {
      if ((node instanceof HTMLAnchorElement || node instanceof HTMLAreaElement) && node.href) {
        return node.href;
      }
    }
    return null;
  };

  const forceHeld = (pressed) => {
    const force = settings?.force;
    if (!force?.enabled) {
      return false;
    }
    const chosen = ['alt', 'shift', 'ctrl', 'insert', 'delete'].filter((name) => force[name] === true);
    return chosen.length > 0 && chosen.every((name) => pressed[name]);
  };

  // Same rule as sendsForcedLinkToLdm in key-rules.js.
  const WEB = /\.(html?|xhtml|shtml|php\d?|aspx?|jsp|cgi|pl|js|mjs|css|jpe?g|png|gif|webp|avif|bmp|ico|svg)$/i;
  const sendsToLdm = (href) => {
    if (settings?.skipWebContent === false) {
      return true;
    }
    let path;
    try {
      path = new URL(href).pathname;
    } catch {
      return false;
    }
    const name = path.slice(path.lastIndexOf('/') + 1);
    const dot = name.lastIndexOf('.');
    return dot > 0 && dot < name.length - 1 && !WEB.test(name);
  };

  const pressedKeys = (event) => ({
    alt: altHeld(event), shift: event.shiftKey, ctrl: event.ctrlKey, insert: keys.insert, delete: keys.delete,
  });

  // Reported on the button press: the browser may start a download (Alt+click in Chrome) before
  // the click event reaches the page.
  addEventListener('mousedown', (event) => {
    const href = event.isTrusted && event.button === 0 ? linkOf(event) : null;
    if (href) {
      send({ type: 'link-click', href, pageUrl: location.href, keys: pressedKeys(event) });
    }
  }, true);

  // A file link clicked with the force keys goes to LDM instead of loading. Other links load as
  // usual; a download they start is forced when it starts. If LDM cannot take the link, the page
  // opens it after all.
  addEventListener('click', (event) => {
    const href = event.isTrusted && event.button === 0 ? linkOf(event) : null;
    if (!enabled || !href || !forceHeld(pressedKeys(event)) || !/^(https?|ftps?):/i.test(href) || !sendsToLdm(href)) {
      return;
    }
    event.preventDefault();
    event.stopImmediatePropagation();
    api.runtime.sendMessage({ type: 'force-link', href, pageUrl: location.href }).then((answer) => {
      if (answer?.handled !== true) {
        location.assign(href);
      }
    }).catch(() => location.assign(href));
  }, true);
})();

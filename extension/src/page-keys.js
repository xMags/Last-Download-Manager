// Runs in every web page (checklist B-03): reports the special keys held and the links clicked to
// the extension, and hands a link clicked with the force keys straight to LDM when the user chose
// that web pages are forced too. A classic script, as content scripts cannot be modules, so the
// force-key test repeats key-rules.js in a few lines. Everything it sends is checked again by the
// background script, because page code runs beside it.
(() => {
  if (globalThis.__ldmPageKeys) {
    return;
  }
  globalThis.__ldmPageKeys = true;
  const api = globalThis.browser ?? globalThis.chrome;
  const keys = { alt: false, shift: false, ctrl: false, insert: false, delete: false };
  let settings = null;  // the "keys" part of LDM's config

  const send = (message) => {
    try {
      api.runtime.sendMessage(message).catch(() => {});
    } catch {
      // the extension was reloaded; this page's script is orphaned
    }
  };

  api.storage.local.get('config').then((stored) => { settings = stored.config?.keys ?? null; }).catch(() => {});
  api.storage.onChanged.addListener((changes, area) => {
    if (area === 'local' && changes.config) {
      settings = changes.config.newValue?.keys ?? null;
    }
  });

  const report = () => send({ type: 'keys', keys: { ...keys } });

  const update = (event, down) => {
    const before = JSON.stringify(keys);
    keys.alt = event.altKey;
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
  addEventListener('keydown', (event) => update(event, true), true);
  addEventListener('keyup', (event) => update(event, false), true);
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

  addEventListener('click', (event) => {
    if (event.button !== 0) {
      return;
    }
    const href = linkOf(event);
    if (!href) {
      return;
    }
    const pressed = { alt: event.altKey, shift: event.shiftKey, ctrl: event.ctrlKey, insert: keys.insert, delete: keys.delete };
    send({ type: 'link-click', href, pageUrl: location.href, keys: pressed });
    // With "let the browser keep web pages" cleared, the forced link goes to LDM instead of loading.
    if (settings && settings.skipWebContent === false && settings.forceNeedsLinkClick !== false &&
        forceHeld(pressed) && /^(https?|ftps?):/i.test(href)) {
      event.preventDefault();
      event.stopImmediatePropagation();
      send({ type: 'force-link', href, pageUrl: location.href });
    }
  }, true);
})();

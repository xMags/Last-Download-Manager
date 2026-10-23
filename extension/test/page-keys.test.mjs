// Runs the real page script (src/page-keys.js) in a sandbox with fake page events.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import vm from 'node:vm';

import { keySettings, sendsForcedLinkToLdm } from '../src/key-rules.js';

const source = readFileSync(new URL('../src/page-keys.js', import.meta.url), 'utf8');

class FakeAnchor {
  constructor(href) {
    this.href = href;
  }
}

async function loadPage({ keys = {}, enabled = true, answer = { handled: true } } = {}) {
  const handlers = {};
  const sent = [];
  const assigned = [];
  const settings = keySettings({ keys });
  const sandbox = {
    URL,
    HTMLAnchorElement: FakeAnchor,
    HTMLAreaElement: class {},
    location: { href: 'https://site.test/list', assign: (url) => assigned.push(url) },
    addEventListener: (type, handler) => { handlers[type] = handler; },
    chrome: {
      storage: {
        local: { get: async () => ({ config: { keys: settings }, enabled }) },
        onChanged: { addListener: () => {} },
      },
      runtime: {
        sendMessage: (message) => {
          sent.push(JSON.parse(JSON.stringify(message)));  // plain objects: the sandbox has its own Object
          return Promise.resolve(message.type === 'force-link' ? answer : undefined);
        },
      },
    },
  };
  sandbox.globalThis = sandbox;
  vm.runInNewContext(source, sandbox);
  await new Promise((resolve) => setImmediate(resolve));  // settings load
  const event = (fields) => {
    const result = { button: 0, altKey: false, shiftKey: false, ctrlKey: false, key: '', prevented: false, ...fields };
    result.composedPath = () => (fields.href ? [new FakeAnchor(fields.href)] : []);
    result.preventDefault = () => { result.prevented = true; };
    result.stopImmediatePropagation = () => {};
    return result;
  };
  const fire = (type, fields) => {
    const e = event(fields);
    handlers[type](e);
    return e;
  };
  return { fire, sent, assigned, settings };
}

test('a press on a link reports it with the keys held, before the browser acts', async () => {
  const page = await loadPage();
  page.fire('mousedown', { href: 'https://site.test/a.zip', altKey: true });
  assert.deepEqual(page.sent.at(-1), {
    type: 'link-click', href: 'https://site.test/a.zip', pageUrl: 'https://site.test/list',
    keys: { alt: true, shift: false, ctrl: false, insert: false, delete: false },
  });
});

test('the right Alt key counts as Alt where it is AltGr, as Firefox reports it', async () => {
  const page = await loadPage();
  const altGraph = (name) => name === 'AltGraph';
  page.fire('keydown', { key: 'AltGraph', getModifierState: altGraph });
  assert.equal(page.sent.at(-1).keys.alt, true, 'held');
  page.fire('mousedown', { href: 'https://site.test/a.zip', getModifierState: altGraph });
  assert.equal(page.sent.at(-1).keys.alt, true, 'clicked');
  page.fire('keyup', { key: 'AltGraph', getModifierState: () => false });
  assert.equal(page.sent.at(-1).keys.alt, false, 'released');
});

test('held keys are reported as they change', async () => {
  const page = await loadPage();
  page.fire('keydown', { key: 'Insert' });
  assert.deepEqual(page.sent.at(-1), { type: 'keys', keys: { alt: false, shift: false, ctrl: false, insert: true, delete: false } });
  page.fire('keyup', { key: 'Insert' });
  assert.equal(page.sent.at(-1).keys.insert, false);
});

test('a file link clicked with the force key goes to LDM instead of loading', async () => {
  const page = await loadPage();
  page.fire('keydown', { key: 'Insert' });
  const click = page.fire('click', { href: 'https://site.test/notes.txt' });
  assert.equal(click.prevented, true);
  assert.deepEqual(page.sent.at(-1), { type: 'force-link', href: 'https://site.test/notes.txt', pageUrl: 'https://site.test/list' });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(page.assigned, [], 'LDM took it');
});

test('a forced link LDM could not take opens in the page after all', async () => {
  const page = await loadPage({ answer: { handled: false } });
  page.fire('keydown', { key: 'Insert' });
  page.fire('click', { href: 'https://site.test/notes.txt' });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(page.assigned, ['https://site.test/notes.txt']);
});

test('without the keys, or with integration off, links load as usual', async () => {
  const page = await loadPage();
  assert.equal(page.fire('click', { href: 'https://site.test/notes.txt' }).prevented, false);
  const off = await loadPage({ enabled: false });
  off.fire('keydown', { key: 'Insert' });
  assert.equal(off.fire('click', { href: 'https://site.test/notes.txt' }).prevented, false);
});

test('the page script and key-rules.js agree on which forced links go to LDM', async () => {
  const links = ['https://site.test/notes.txt', 'https://site.test/a.zip', 'https://site.test/page.html',
    'https://site.test/photo.jpg', 'https://site.test/download?id=3', 'https://site.test/dir/', 'https://site.test/x.php?f=a.zip'];
  for (const keys of [{}, { skipWebContent: false }]) {
    const page = await loadPage({ keys });
    page.fire('keydown', { key: 'Insert' });
    for (const href of links) {
      const prevented = page.fire('click', { href }).prevented;
      assert.equal(prevented, sendsForcedLinkToLdm(page.settings, href), `${href} with ${JSON.stringify(keys)}`);
    }
  }
});

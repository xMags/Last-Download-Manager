import assert from 'node:assert/strict';
import { test } from 'node:test';

import { PanelTabs } from '../src/panel-tabs.js';

function sessionStorage(initial) {
  const data = { ...initial };
  return {
    data,
    session: {
      get: async (key) => (key in data ? { [key]: data[key] } : {}),
      set: async (values) => { Object.assign(data, values); },
    },
  };
}

test('a click turns the panel off in one tab and the next turns it on again', async () => {
  const storage = sessionStorage({});
  const tabs = new PanelTabs({ storage });
  assert.equal(await tabs.toggle(4), true);
  assert.equal(tabs.isOff(4), true);
  assert.equal(tabs.isOff(5), false, 'other tabs keep their panel');
  assert.equal(await tabs.toggle(4), false);
  assert.equal(tabs.isOff(4), false);
  assert.deepEqual(tabs.switchedTabs(), [4], 'the tab keeps a look of its own until it closes');
  await tabs.forget(4);
  assert.deepEqual(tabs.switchedTabs(), []);
});

test('the switched tabs survive the background script stopping', async () => {
  const storage = sessionStorage({});
  await new PanelTabs({ storage }).toggle(9);
  const restarted = new PanelTabs({ storage });
  await restarted.ready;
  assert.equal(restarted.isOff(9), true);
});

test('stored junk is ignored, and a browser without session storage still works', async () => {
  const junk = new PanelTabs({ storage: sessionStorage({ panelTabs: [[1, true], ['x', true], [2, 'yes'], 3] }) });
  await junk.ready;
  assert.deepEqual(junk.switchedTabs(), [1]);
  const bare = new PanelTabs({ storage: {} });
  await bare.ready;
  assert.equal(await bare.toggle(1), true);
});

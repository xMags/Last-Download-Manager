import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';

import { State } from '../src/connection.js';
import { ICONS, toolbarAppearance } from '../src/toolbar.js';

test('turned off shows the grey icon and [disabled]', () => {
  const look = toolbarAppearance({ enabled: false, connection: State.Connected });
  assert.equal(look.icon, 'off');
  assert.equal(look.badge, '');
  assert.match(look.title, /\[disabled\]/);
});

test('a missing host shows [error] with a badge', () => {
  const look = toolbarAppearance({ enabled: true, connection: State.Error });
  assert.equal(look.icon, 'on');
  assert.equal(look.badge, '!');
  assert.match(look.title, /\[error\]/);
});

test('LDM not running is not an error: capture still starts it', () => {
  const look = toolbarAppearance({ enabled: true, connection: State.NotRunning });
  assert.equal(look.badge, '');
  assert.doesNotMatch(look.title, /\[error\]/);
  assert.match(look.title, /not running/);
});

test('connected shows the plain icon', () => {
  const look = toolbarAppearance({ enabled: true, connection: State.Connected });
  assert.equal(look.icon, 'on');
  assert.equal(look.badge, '');
  assert.match(look.title, /click to turn/);
});

test('icon paths are absolute and the files exist', () => {
  for (const set of Object.values(ICONS)) {
    for (const path of Object.values(set)) {
      assert.ok(path.startsWith('/'), `${path} must start at the extension root`);
      assert.ok(existsSync(fileURLToPath(new URL(`..${path}`, import.meta.url))), `${path} is missing`);
    }
  }
});

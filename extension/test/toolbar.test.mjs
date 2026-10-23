import assert from 'node:assert/strict';
import { test } from 'node:test';

import { State } from '../src/connection.js';
import { toolbarAppearance } from '../src/toolbar.js';

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

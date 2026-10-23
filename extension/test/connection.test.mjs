import assert from 'node:assert/strict';
import { test } from 'node:test';

import { classifyDisconnect, LdmConnection, State } from '../src/connection.js';
import { browserInfo } from '../src/browser-info.js';

// A stand-in for runtime.connectNative's port.
class FakePort {
  constructor() {
    this.sent = [];
    this.messageListeners = [];
    this.disconnectListeners = [];
    this.onMessage = { addListener: (listener) => this.messageListeners.push(listener) };
    this.onDisconnect = { addListener: (listener) => this.disconnectListeners.push(listener) };
  }

  postMessage(message) {
    this.sent.push(message);
  }

  receive(message) {
    this.messageListeners.forEach((listener) => listener(message));
  }

  disconnect(api, errorText) {
    api.runtime.lastError = errorText ? { message: errorText } : undefined;
    this.disconnectListeners.forEach((listener) => listener(this));
    api.runtime.lastError = undefined;
  }
}

function fakeApi() {
  const api = { ports: [], runtime: {} };
  api.runtime.connectNative = (name) => {
    assert.equal(name, 'io.github.mrdevfx.ldm');
    const port = new FakePort();
    api.ports.push(port);
    return port;
  };
  return api;
}

function newConnection(api) {
  const log = { configs: [], states: [] };
  const connection = new LdmConnection(api, {
    helloInfo: async () => ({ browser: 'Chrome', browserVersion: '141', extensionVersion: '5.0.0', incognitoAllowed: false }),
    onConfig: (config) => log.configs.push(config),
    onState: (state) => log.states.push(state),
  });
  return { connection, log };
}

const CONFIG = { v: 1, id: 'm1', type: 'config', appVersion: '5.0.0', capturedExtensions: ['zip'] };

test('connect sends hello and becomes connected on config', async () => {
  const api = fakeApi();
  const { connection, log } = newConnection(api);
  const ready = connection.connect();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(api.ports.length, 1);
  const [hello] = api.ports[0].sent;
  assert.equal(hello.type, 'hello');
  assert.equal(hello.startLdm, false);
  api.ports[0].receive(CONFIG);
  assert.equal(await ready, true);
  assert.equal(connection.state, State.Connected);
  assert.deepEqual(log.states, [State.Connecting, State.Connected]);
  assert.equal(log.configs.length, 1);
  assert.deepEqual(log.configs[0].capturedExtensions, ['zip']);
});

test('a second connect while connecting reuses the same attempt', async () => {
  const api = fakeApi();
  const { connection } = newConnection(api);
  const first = connection.connect();
  const second = connection.connect({ startLdm: true });
  assert.equal(first, second);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(api.ports.length, 1);
});

test('startLdm is passed to the host only when asked', async () => {
  const api = fakeApi();
  const { connection } = newConnection(api);
  connection.connect({ startLdm: true });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(api.ports[0].sent[0].startLdm, true);
});

test('host exit means LDM is not running; a later connect starts over', async () => {
  const api = fakeApi();
  const { connection } = newConnection(api);
  const ready = connection.connect();
  await new Promise((resolve) => setImmediate(resolve));
  api.ports[0].disconnect(api, 'Native host has exited.');
  assert.equal(await ready, false);
  assert.equal(connection.state, State.NotRunning);
  connection.connect();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(api.ports.length, 2);
});

test('missing or forbidden host is an error', () => {
  assert.equal(classifyDisconnect('Specified native messaging host not found.'), State.Error);
  assert.equal(classifyDisconnect('Access to the specified native messaging host is forbidden.'), State.Error);
  assert.equal(classifyDisconnect('No such native application io.github.mrdevfx.ldm'), State.Error);
  assert.equal(classifyDisconnect('Native host has exited.'), State.NotRunning);
  assert.equal(classifyDisconnect(undefined), State.NotRunning);
});

test('messages that are not a config do not connect', async () => {
  const api = fakeApi();
  const { connection } = newConnection(api);
  connection.connect();
  await new Promise((resolve) => setImmediate(resolve));
  api.ports[0].receive({ v: 1, type: 'config' });
  assert.equal(connection.state, State.Connecting);
});

test('browser names come from Chromium brands or Firefox info', async () => {
  const chromium = { runtime: {} };
  assert.deepEqual(
    await browserInfo(chromium, { userAgentData: { brands: [{ brand: 'Not A Brand', version: '8' },
      { brand: 'Chromium', version: '141' }, { brand: 'Microsoft Edge', version: '141' }] } }),
    { browser: 'Edge', browserVersion: '141' });
  assert.deepEqual(await browserInfo(chromium, {}), { browser: 'Chromium', browserVersion: '' });
  const firefox = { runtime: { getBrowserInfo: async () => ({ name: 'Firefox', version: '144.0' }) } };
  assert.deepEqual(await browserInfo(firefox, {}), { browser: 'Firefox', browserVersion: '144.0' });
});

test('requests get the answer with their id, and nothing once the connection ends', async () => {
  const api = fakeApi();
  const { connection } = newConnection(api);
  const ready = connection.connect();
  await new Promise((resolve) => setImmediate(resolve));
  api.ports[0].receive(CONFIG);
  await ready;

  const answered = connection.request({ v: 1, id: 'c1', type: 'capture' }, 1000);
  api.ports[0].receive({ v: 1, id: 'other', type: 'capture-result', accepted: true });
  api.ports[0].receive({ v: 1, id: 'c1', type: 'capture-result', accepted: true });
  assert.equal((await answered).id, 'c1');

  const orphaned = connection.request({ v: 1, id: 'c2', type: 'capture' }, 1000);
  api.ports[0].disconnect(api, 'Native host has exited.');
  assert.equal(await orphaned, null);
  assert.equal(await connection.request({ v: 1, id: 'c3', type: 'capture' }, 1000), null, 'not connected');
});

test('an unanswered request times out', async () => {
  const api = fakeApi();
  const { connection } = newConnection(api);
  const ready = connection.connect();
  await new Promise((resolve) => setImmediate(resolve));
  api.ports[0].receive(CONFIG);
  await ready;
  assert.equal(await connection.request({ v: 1, id: 'slow', type: 'capture' }, 20), null);
});

test('one-shot messages use a connection of their own that asks to start LDM', async () => {
  const api = fakeApi();
  const { connection } = newConnection(api);
  const answered = connection.oneShot({ v: 1, id: 'c9', type: 'capture' }, 1000);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(api.ports.length, 1);
  const port = api.ports[0];
  port.disconnectCalls = 0;
  port.disconnect = () => { port.disconnectCalls += 1; };
  assert.equal(port.sent[0].type, 'hello');
  assert.equal(port.sent[0].startLdm, true);
  assert.equal(port.sent[1].id, 'c9');
  port.receive(CONFIG);  // the config every hello gets is not the answer
  port.receive({ v: 1, id: 'c9', type: 'capture-result', accepted: true });
  assert.equal((await answered).accepted, true);
  assert.equal(port.disconnectCalls, 1, 'closed once answered');
  assert.equal(connection.state, State.NotRunning, 'the persistent connection is untouched');
});

test('a one-shot connection that ends without an answer gives null', async () => {
  const api = fakeApi();
  const { connection } = newConnection(api);
  const answered = connection.oneShot({ v: 1, id: 'c10', type: 'capture' }, 1000);
  await new Promise((resolve) => setImmediate(resolve));
  api.ports[0].disconnect = () => {};
  api.ports[0].disconnectListeners.forEach((listener) => listener(api.ports[0]));
  assert.equal(await answered, null);
});

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { cookieStoreFor, privateStoreId } from '../src/cookie-stores.js';

function api(stores, tabs) {
  return {
    cookies: { getAllCookieStores: async () => stores },
    tabs: { get: async (id) => tabs[id] ?? Promise.reject(new Error('no tab')) },
  };
}

test('the private store is found by its tabs in Chrome and by its mark in Firefox', async () => {
  const chrome = api([{ id: '0', tabIds: [1] }, { id: '1', tabIds: [7] }], { 1: { incognito: false }, 7: { incognito: true } });
  assert.equal(await privateStoreId(chrome), '1');
  const firefox = api([{ id: 'firefox-default', tabIds: [] }, { id: 'firefox-private', incognito: true, tabIds: [] }], {});
  assert.equal(await privateStoreId(firefox), 'firefox-private');
  assert.equal(await privateStoreId(api([{ id: '0', tabIds: [1] }], { 1: { incognito: false } })), null);
});

test('normal windows use the normal store; a private one without its store gives null', async () => {
  const chrome = api([{ id: '0', tabIds: [1] }], { 1: { incognito: false } });
  assert.equal(await cookieStoreFor(chrome, { incognito: false }), undefined);
  assert.equal(await cookieStoreFor(chrome, undefined), undefined);
  assert.equal(await cookieStoreFor(chrome, { incognito: true }), null);
  assert.equal(await cookieStoreFor(chrome, { incognito: true, cookieStoreId: 'firefox-container-2' }), 'firefox-container-2');
});

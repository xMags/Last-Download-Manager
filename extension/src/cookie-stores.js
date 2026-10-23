// Which cookie store a download or a page uses (checklist B-11). Private windows have their own
// store; reading cookies from the normal one would hand LDM the wrong session, or none.

/**
 * The cookie store id of the browser's private windows, or null when none is open. Firefox marks
 * the store; Chrome and Edge only list the tabs that use each store.
 * @param {object} api the extension API object
 */
export async function privateStoreId(api) {
  let stores = [];
  try {
    stores = (await api.cookies.getAllCookieStores()) ?? [];
  } catch {
    return null;
  }
  const marked = stores.find((store) => store.incognito === true);
  if (marked) {
    return marked.id;
  }
  for (const store of stores) {
    for (const tabId of store.tabIds ?? []) {
      const tab = await api.tabs.get(tabId).catch(() => null);
      if (tab?.incognito) {
        return store.id;
      }
    }
  }
  return null;
}

/**
 * The cookie store for something that happened in a normal or a private window.
 * @param {object} api the extension API object
 * @param {{incognito?: boolean, cookieStoreId?: string}} source a DownloadItem or a Tab
 * @returns {Promise<string | undefined | null>} undefined for the normal store, null when the
 *   private store cannot be found
 */
export async function cookieStoreFor(api, source) {
  if (typeof source?.cookieStoreId === 'string' && source.cookieStoreId) {
    return source.cookieStoreId;  // Firefox tabs name theirs, containers included
  }
  if (!source?.incognito) {
    return undefined;
  }
  return privateStoreId(api);
}

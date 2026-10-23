// Which browser this is, for LDM's Options > General list.

const CHROMIUM_BRANDS = [
  ['Microsoft Edge', 'Edge'],
  ['Google Chrome', 'Chrome'],
  ['Opera', 'Opera'],
  ['Brave', 'Brave'],
  ['Chromium', 'Chromium'],
];

/**
 * @param {object} api the extension API object (browser or chrome)
 * @param {object} nav the navigator
 * @returns {Promise<{browser: string, browserVersion: string}>}
 */
export async function browserInfo(api, nav) {
  if (typeof api.runtime.getBrowserInfo === 'function') {
    const info = await api.runtime.getBrowserInfo();  // Firefox
    return { browser: info.name, browserVersion: info.version };
  }
  const brands = nav.userAgentData?.brands ?? [];
  for (const [brand, name] of CHROMIUM_BRANDS) {
    const match = brands.find((entry) => entry.brand === brand);
    if (match) {
      return { browser: name, browserVersion: match.version };
    }
  }
  return { browser: 'Chromium', browserVersion: '' };
}

// Messages between this extension and LDM (M3 design section 4). LDM validates everything it
// receives; this side validates what LDM sends, so a broken or foreign host cannot feed the
// extension nonsense.

export const PROTOCOL_VERSION = 1;
export const HOST_NAME = 'io.github.mrdevfx.ldm';

const MAX_NAME = 64;
const MAX_VERSION = 32;
const EXTENSION_PATTERN = /^[a-z0-9*?_+~-]{1,32}$/;

let lastId = 0;

/** A fresh request id, unique while the extension runs. */
export function nextId() {
  lastId += 1;
  return `m${lastId}`;
}

/** Printable ASCII only, cut to `max` characters: LDM rejects anything else. */
export function cleanText(value, max) {
  return String(value ?? '').replace(/[^\x20-\x7e]/g, '').slice(0, max);
}

/**
 * The first message on every connection.
 * @param {{browser: string, browserVersion: string, extensionVersion: string,
 *          incognitoAllowed: boolean, startLdm: boolean}} info
 */
export function makeHello(info) {
  return {
    v: PROTOCOL_VERSION,
    id: nextId(),
    type: 'hello',
    browser: cleanText(info.browser, MAX_NAME) || 'Browser',
    browserVersion: cleanText(info.browserVersion, MAX_VERSION),
    extensionVersion: cleanText(info.extensionVersion, MAX_VERSION) || '0',
    incognitoAllowed: Boolean(info.incognitoAllowed),
    startLdm: Boolean(info.startLdm),
  };
}

/**
 * The capture rules from a config message, or null when the message is not a valid config.
 * Invalid list entries are dropped rather than failing the whole config.
 * @returns {{appVersion: string, capturedExtensions: string[]} | null}
 */
export function parseConfig(message) {
  if (message === null || typeof message !== 'object' || message.v !== PROTOCOL_VERSION ||
      message.type !== 'config' || typeof message.appVersion !== 'string' ||
      !Array.isArray(message.capturedExtensions)) {
    return null;
  }
  return {
    appVersion: cleanText(message.appVersion, MAX_VERSION),
    capturedExtensions: message.capturedExtensions.filter(
      (entry) => typeof entry === 'string' && EXTENSION_PATTERN.test(entry)),
  };
}

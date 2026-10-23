// The native messaging connection to LDM through ldm-host.exe (M3 design section 3).

import { HOST_NAME, makeHello, parseConfig } from './protocol.js';

/**
 * Connection states, as the toolbar shows them.
 * - connecting: waiting for LDM's answer
 * - connected: LDM answered with its rules
 * - not-running: the host started but LDM is not running (or exited)
 * - error: the browser cannot start the host at all (LDM never ran, or it was moved or removed)
 */
export const State = Object.freeze({
  Connecting: 'connecting',
  Connected: 'connected',
  NotRunning: 'not-running',
  Error: 'error',
});

/** Chrome, Edge and Firefox wordings for "this host is not registered or not allowed". */
const HOST_MISSING = /not found|no such native application|forbidden|not allowed|access/i;

/** What a disconnect means, from the browser's error text. */
export function classifyDisconnect(errorText) {
  return HOST_MISSING.test(errorText ?? '') ? State.Error : State.NotRunning;
}

export class LdmConnection {
  /**
   * @param {object} api the extension API object
   * @param {{onConfig: function, onState: function, helloInfo: function(): Promise<object>}} hooks
   *   helloInfo supplies the browser fields of the hello.
   */
  constructor(api, hooks) {
    this.api = api;
    this.hooks = hooks;
    this.port = null;
    this.ready = null;
    this.state = State.NotRunning;
    this.pending = new Map();  // request id -> resolve function, answered by LDM
  }

  /**
   * Sends a message that LDM answers (an answer carries the same id) and resolves with the answer,
   * or with null when the connection ends or no answer comes within `timeoutMs`.
   */
  request(message, timeoutMs) {
    if (!this.port || this.state !== State.Connected) {
      return Promise.resolve(null);
    }
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.pending.delete(message.id);
        resolve(null);
      }, timeoutMs);
      this.pending.set(message.id, (answer) => {
        clearTimeout(timer);
        resolve(answer);
      });
      try {
        this.port.postMessage(message);
      } catch {
        this.pending.get(message.id)?.(null);
        this.pending.delete(message.id);
      }
    });
  }

  /**
   * Connects unless already connected or connecting. Resolves true once LDM sent its rules, false
   * when the connection ended first.
   * @param {{startLdm?: boolean}} options startLdm lets the host start LDM; only for real work.
   */
  connect({ startLdm = false } = {}) {
    if (this.ready) {
      return this.ready;
    }
    this.setState(State.Connecting);
    this.ready = this.hooks.helloInfo().then((info) => this.open({ ...info, startLdm }));
    return this.ready;
  }

  /** @private */
  open(helloFields) {
    return new Promise((resolve) => {
      let port;
      try {
        port = this.api.runtime.connectNative(HOST_NAME);
      } catch (error) {
        this.closed(classifyDisconnect(String(error?.message ?? error)));
        resolve(false);
        return;
      }
      this.port = port;
      port.onMessage.addListener((message) => {
        const config = parseConfig(message);
        if (config) {
          this.setState(State.Connected);
          this.hooks.onConfig(config);
          resolve(true);
          return;
        }
        const answer = typeof message?.id === 'string' ? this.pending.get(message.id) : undefined;
        if (answer) {
          this.pending.delete(message.id);
          answer(message);
        }
      });
      port.onDisconnect.addListener((disconnected) => {
        // Chrome reports through runtime.lastError, Firefox through port.error.
        const text = this.api.runtime.lastError?.message ?? disconnected?.error?.message ?? '';
        this.closed(classifyDisconnect(text));
        resolve(false);
      });
      port.postMessage(makeHello(helloFields));
    });
  }

  /** @private */
  closed(state) {
    this.port = null;
    this.ready = null;
    for (const answer of this.pending.values()) {
      answer(null);
    }
    this.pending.clear();
    this.setState(state);
  }

  /** @private */
  setState(state) {
    if (this.state !== state) {
      this.state = state;
      this.hooks.onState(state);
    }
  }
}

// The native messaging connection to LDM through ldm-host.exe (M3 design section 3).

import { HOST_NAME, makeHello, parseConfig } from './protocol.js';

/**
 * Connection states, as the toolbar shows them.
 * - connecting: waiting for LDM's answer
 * - connected: LDM answered with its rules
 * - not-running: the host started but LDM is not running (or exited)
 * - error: the browser cannot start the host at all (LDM never ran, or it was moved or removed)
 * - elevated: LDM runs as administrator, so the host may not connect to it
 */
export const State = Object.freeze({
  Connecting: 'connecting',
  Connected: 'connected',
  NotRunning: 'not-running',
  Error: 'error',
  Elevated: 'elevated',
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
   * @param {{onConfig: function, onState: function, helloInfo: function(): Promise<object>,
   *          onMessage?: function(object)}} hooks
   *   helloInfo supplies the browser fields of the hello; onMessage gets what LDM sends unasked.
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
   * Sends a message LDM does not answer on the lasting connection. False when not connected: the
   * message is dropped, as media reports must never start LDM.
   */
  post(message) {
    if (!this.port || this.state !== State.Connected || message === null) {
      return false;
    }
    try {
      this.port.postMessage(message);
      return true;
    } catch {
      return false;
    }
  }

  /**
   * Sends one message on a connection of its own and resolves with LDM's answer (same id), or with
   * null when there is none within `timeoutMs`. Each call starts a fresh ldm-host.exe, and LDM when
   * it is not running. Captures use this: the browser starts that host while the user is in the
   * browser, which is what lets the host allow LDM's dialog to come to the front.
   */
  async oneShot(message, timeoutMs) {
    const hello = makeHello({ ...(await this.hooks.helloInfo()), startLdm: true });
    return new Promise((resolve) => {
      let port;
      let settled = false;
      const finish = (answer) => {
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(timer);
        try {
          port?.disconnect();
        } catch {
          // already gone
        }
        resolve(answer);
      };
      const timer = setTimeout(() => finish(null), timeoutMs);
      try {
        port = this.api.runtime.connectNative(HOST_NAME);
      } catch {
        finish(null);
        return;
      }
      port.onMessage.addListener((answer) => {
        if (answer?.id === message.id) {
          finish(answer);
        }
      });
      port.onDisconnect.addListener(() => finish(null));
      port.postMessage(hello);
      port.postMessage(message);
    });
  }

  /**
   * Like oneShot, but the connection stays open after LDM's answer, for messages that follow on
   * it (Download All Links: link details towards LDM, links-done back).
   * @param {{onMessage: function(object), onEnd: function()}} hooks for after the answer: each
   *   later message from LDM, and the end of the connection (from either side, once)
   * @returns {Promise<{answer: object, send: function(object), close: function()} | null>}
   *   null when no answer came within `timeoutMs`; the connection is closed then
   */
  async conversation(message, timeoutMs, hooks) {
    const hello = makeHello({ ...(await this.hooks.helloInfo()), startLdm: true });
    return new Promise((resolve) => {
      let port;
      let answered = false;
      let ended = false;
      const end = () => {
        if (ended) {
          return;
        }
        ended = true;
        clearTimeout(timer);
        try {
          port?.disconnect();
        } catch {
          // already gone
        }
        if (answered) {
          hooks.onEnd();
        } else {
          resolve(null);
        }
      };
      const timer = setTimeout(end, timeoutMs);
      try {
        port = this.api.runtime.connectNative(HOST_NAME);
      } catch {
        end();
        return;
      }
      port.onMessage.addListener((received) => {
        if (ended) {
          return;
        }
        if (answered) {
          hooks.onMessage(received);
          return;
        }
        if (received?.id === message.id) {
          answered = true;
          clearTimeout(timer);
          resolve({
            answer: received,
            send: (next) => {
              if (!ended) {
                try {
                  port.postMessage(next);
                } catch {
                  end();
                }
              }
            },
            close: end,
          });
        }
      });
      port.onDisconnect.addListener(end);
      port.postMessage(hello);
      port.postMessage(message);
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
      let hostError = null;  // what the host said before it disconnected
      port.onMessage.addListener((message) => {
        if (message?.type === 'host-error') {
          hostError = message.reason === 'ldm-elevated' ? State.Elevated : null;
          return;
        }
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
          return;
        }
        this.hooks.onMessage?.(message);
      });
      port.onDisconnect.addListener((disconnected) => {
        // Chrome reports through runtime.lastError, Firefox through port.error.
        const text = this.api.runtime.lastError?.message ?? disconnected?.error?.message ?? '';
        this.closed(hostError ?? classifyDisconnect(text));
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

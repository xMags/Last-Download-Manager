// Finds out the type, size and server-given name of the links in an open Download All Links dialog
// (checklist B-05), by asking each server for the headers only. Runs in the background with the
// browser's cookies, as the page's own requests would. Stops when the dialog closes.

const CONCURRENCY = 6;
const REQUEST_TIMEOUT_MS = 10000;
// Details go to LDM in small batches, so the list fills in steadily without a message per link.
const BATCH_DELAY_MS = 250;
const BATCH_MAX = 50;

/**
 * The file name in a Content-Disposition header value, or undefined. RFC 6266: filename* (RFC 8187,
 * percent-encoded UTF-8) wins over filename.
 */
export function fileNameFromDisposition(value) {
  if (typeof value !== 'string' || value === '') {
    return undefined;
  }
  const extended = /filename\*\s*=\s*([^']*)'[^']*'([^;]+)/i.exec(value);
  if (extended) {
    try {
      const charset = extended[1].trim().toLowerCase();
      const raw = extended[2].trim().replace(/^"|"$/g, '');
      if (charset === 'utf-8' || charset === '') {
        return decodeURIComponent(raw) || undefined;
      }
      // ISO-8859-1: each %XX is one character.
      return raw.replace(/%([0-9a-f]{2})/gi, (match, hex) => String.fromCharCode(parseInt(hex, 16))) || undefined;
    } catch {
      // malformed: fall back to the plain parameter
    }
  }
  const quoted = /filename\s*=\s*"((?:[^"\\]|\\.)*)"/i.exec(value);
  if (quoted) {
    return quoted[1].replace(/\\(.)/g, '$1') || undefined;
  }
  const plain = /filename\s*=\s*([^;\s]+)/i.exec(value);
  return plain ? plain[1] : undefined;
}

/** The details LDM shows from a response's headers. */
export function detailsFromHeaders(headers) {
  const length = headers.get('content-length');
  const size = length !== null && /^\d+$/.test(length.trim()) ? Number(length.trim()) : undefined;
  return {
    fileSize: Number.isSafeInteger(size) ? size : undefined,
    mimeType: headers.get('content-type') ?? undefined,
    fileName: fileNameFromDisposition(headers.get('content-disposition') ?? ''),
  };
}

export class LinkChecker {
  /**
   * @param {function(string, object): Promise<Response>} fetchFn the global fetch
   * @param {function(object[])} send receives batches of {index, failed} or {index, fileSize,
   *   mimeType, fileName}
   */
  constructor(fetchFn, send) {
    this.fetch = fetchFn;
    this.send = send;
    this.stopped = false;
    this.controllers = new Set();
    this.batch = [];
    this.timer = null;
  }

  /**
   * Checks every address once; links sharing an address get the same answer. Addresses that are
   * not http or https (ftp) are reported without details.
   * @param {string[]} urls in the order of the links message
   */
  async run(urls) {
    const indexes = new Map();
    urls.forEach((url, index) => {
      const key = url.split('#')[0];
      indexes.set(key, [...(indexes.get(key) ?? []), index]);
    });
    const queue = [...indexes.entries()];
    const worker = async () => {
      while (!this.stopped && queue.length > 0) {
        const [url, those] = queue.shift();
        const details = /^https?:/i.test(url) ? await this.check(url) : {};
        if (this.stopped) {
          return;
        }
        for (const index of those) {
          this.report({ index, ...details });
        }
      }
    };
    await Promise.all(Array.from({ length: CONCURRENCY }, worker));
    this.flush();
  }

  /** Stops checking: requests in flight are aborted and nothing more is sent. */
  stop() {
    this.stopped = true;
    clearTimeout(this.timer);
    this.batch = [];
    for (const controller of this.controllers) {
      controller.abort();
    }
  }

  /** @private The headers of `url`: HEAD first; GET (aborted after the headers) for servers that refuse HEAD. */
  async check(url) {
    for (const method of ['HEAD', 'GET']) {
      const controller = new AbortController();
      this.controllers.add(controller);
      const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
      try {
        const response = await this.fetch(url, {
          method, credentials: 'include', redirect: 'follow', cache: 'no-store', signal: controller.signal,
        });
        if (method === 'HEAD' && (response.status === 405 || response.status === 501 || response.status === 403)) {
          continue;
        }
        if (!response.ok) {
          return { failed: true };
        }
        return detailsFromHeaders(response.headers);
      } catch {
        // A HEAD the network refused may still work as GET; one that timed out would only time out again.
        if (this.stopped || method === 'GET' || controller.signal.aborted) {
          return { failed: true };
        }
      } finally {
        clearTimeout(timer);
        controller.abort();  // a GET's body is not needed
        this.controllers.delete(controller);
      }
    }
    return { failed: true };
  }

  /** @private */
  report(item) {
    this.batch.push(item);
    if (this.batch.length >= BATCH_MAX) {
      this.flush();
    } else if (!this.timer) {
      this.timer = setTimeout(() => this.flush(), BATCH_DELAY_MS);
    }
  }

  /** @private */
  flush() {
    clearTimeout(this.timer);
    this.timer = null;
    if (this.stopped || this.batch.length === 0) {
      return;
    }
    const items = this.batch;
    this.batch = [];
    this.send(items);
  }
}

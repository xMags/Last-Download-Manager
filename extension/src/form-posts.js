// Remembers recent form submissions (POST) of pages, so that a download answering one can be
// handed to LDM together with the form (checklist ENG-11). The browser's downloads API does not
// say how a download was requested, so webRequest observes the submissions.

import { replacesPage } from './media-rules.js';

const LIFETIME_MS = 30000;
const MAX_POSTS = 20;
export const MAX_BODY_BYTES = 1024 * 1024;

/** Base64 of bytes, in chunks small enough for String.fromCharCode. */
export function toBase64(bytes) {
  let binary = '';
  for (let i = 0; i < bytes.length; i += 0x8000) {
    binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  }
  return btoa(binary);
}

/**
 * The body of a submission from webRequest's requestBody, or null when it cannot be rebuilt.
 * Forms arrive as parsed fields (formData) or as raw bytes; files chosen in a form are never
 * available to extensions, so a form with a file is not rebuilt.
 * @returns {{contentType: string, bytes: Uint8Array} | null}
 */
export function rebuildBody(requestBody, contentType) {
  if (!requestBody || requestBody.error) {
    return null;
  }
  const type = String(contentType ?? '').toLowerCase();
  if (Array.isArray(requestBody.raw)) {
    if (requestBody.raw.some((part) => !part.bytes)) {
      return null;  // a file part, given only by name
    }
    const parts = requestBody.raw.map((part) => new Uint8Array(part.bytes));
    const size = parts.reduce((total, part) => total + part.length, 0);
    const bytes = new Uint8Array(size);
    let offset = 0;
    for (const part of parts) {
      bytes.set(part, offset);
      offset += part.length;
    }
    return { contentType: contentType || 'application/octet-stream', bytes };
  }
  if (requestBody.formData && typeof requestBody.formData === 'object') {
    if (type.startsWith('multipart/form-data')) {
      return null;  // its file fields are given only by name
    }
    const params = new URLSearchParams();
    for (const [name, values] of Object.entries(requestBody.formData)) {
      for (const value of [].concat(values)) {
        params.append(name, String(value));
      }
    }
    return { contentType: 'application/x-www-form-urlencoded', bytes: new TextEncoder().encode(params.toString()) };
  }
  return null;
}

export class FormPosts {
  constructor(now = () => Date.now()) {
    this.now = now;
    this.pending = new Map();  // requestId -> { url, requestBody } until its headers are seen
    this.posts = [];
  }

  /** webRequest.onBeforeRequest with "requestBody". */
  onBeforeRequest(details) {
    if (details.method === 'POST' && details.requestBody) {
      this.pending.set(details.requestId, { url: details.url, requestBody: details.requestBody });
      if (this.pending.size > MAX_POSTS) {
        this.pending.delete(this.pending.keys().next().value);
      }
    }
  }

  /** webRequest.onSendHeaders with "requestHeaders": the Content-Type completes the submission. */
  onSendHeaders(details) {
    const started = this.pending.get(details.requestId);
    if (!started) {
      return;
    }
    this.pending.delete(details.requestId);
    const contentType = (details.requestHeaders ?? []).find((header) => header.name.toLowerCase() === 'content-type')?.value;
    const body = rebuildBody(started.requestBody, contentType);
    if (!body || body.bytes.length > MAX_BODY_BYTES) {
      return;
    }
    this.posts.push({ requestId: details.requestId, url: started.url, ...body, at: this.now() });
    if (this.posts.length > MAX_POSTS) {
      this.posts.shift();
    }
  }

  /**
   * webRequest.onHeadersReceived: a submission answered with a page did not download anything, so
   * it is forgotten at once. A later download of the same address is then not sent as the form
   * again, and the form's fields (a login's too) do not stay in memory.
   */
  onHeadersReceived(details) {
    if (replacesPage(details)) {
      this.posts = this.posts.filter((post) => post.requestId !== details.requestId);
    }
  }

  /** The newest recent submission to this address, for a capture message; null when there is none. */
  postFor(url) {
    const now = this.now();
    this.posts = this.posts.filter((post) => now - post.at <= LIFETIME_MS);
    for (let i = this.posts.length - 1; i >= 0; i -= 1) {
      if (this.posts[i].url === url) {
        return { contentType: this.posts[i].contentType, body: toBase64(this.posts[i].bytes) };
      }
    }
    return null;
  }
}

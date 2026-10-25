import { makeVideoPosition } from './protocol.js';

// Tells LDM where the video of each tab it has media for is (M3 design section 6). The frames of
// the tab report their videos (src/video-finder.js); this picks the best one and adds what only
// the background knows: whether the tab is the active one and where its window is. A tab where
// the user turned the panel off (src/panel-tabs.js) is not followed and reports no video.

// A frame that has not reported for this long is gone (frames report at least every 3 seconds).
const STALE_MS = 8000;

/**
 * The report to place the panel by: the best video of any frame, with the top frame's viewport.
 * @param {Map<number, {report: object, time: number}>} frames reports by frame id
 * @param {number} now milliseconds
 * @returns {{video: object | null, top: object | null}}
 */
export function chooseVideo(frames, now) {
  let best = null;
  for (const { report, time } of frames.values()) {
    if (now - time <= STALE_MS && report.video && (!best || report.score > best.score)) {
      best = report;
    }
  }
  const top = frames.get(0);
  return { video: best?.video ?? null, top: top && now - top.time <= STALE_MS ? top.report : null };
}

/** A frame's report as video-finder.js sends it, or null when it is malformed. */
export function cleanFrameReport(message, isTopFrame) {
  const rect = (value) => (value && ['x', 'y', 'width', 'height'].every((key) => Number.isFinite(value[key])) &&
    value.width >= 0 && value.height >= 0 ? { x: value.x, y: value.y, width: value.width, height: value.height } : null);
  if (message?.type !== 'video-rect') {
    return null;
  }
  const report = {
    video: message.video === null ? null : rect(message.video),
    score: Number.isFinite(message.score) && message.score >= 0 ? message.score : 0,
    hidden: Boolean(message.hidden),
    fullscreen: Boolean(message.fullscreen),
  };
  if (message.video !== null && !report.video) {
    return null;
  }
  if (isTopFrame) {
    report.viewport = rect(message.viewport);
    report.viewportPositionKnown = Boolean(message.viewportPositionKnown);
    report.pixelRatio = Number.isFinite(message.pixelRatio) ? message.pixelRatio : 1;
    if (!report.viewport) {
      return null;
    }
  }
  return report;
}

export class VideoPositions {
  /**
   * @param {object} api the extension API object
   * @param {{post: function(object): boolean}} connection the lasting connection to LDM
   * @param {function(number): boolean} [isPanelOff] true for a tab where the user turned the panel off
   */
  constructor(api, connection, isPanelOff = () => false) {
    this.api = api;
    this.connection = connection;
    this.isPanelOff = isPanelOff;
    this.tracked = new Set();  // tab ids LDM has media for
    this.frames = new Map();   // tab id -> Map(frame id -> {report, time})
    this.lastSent = new Map(); // tab id -> the last position sent, as JSON
  }

  /** LDM's media-list: start or stop following a tab's video. */
  setHasMedia(tabId, hasMedia) {
    if (hasMedia) {
      this.tracked.add(tabId);
    } else {
      this.tracked.delete(tabId);
      this.frames.delete(tabId);
      this.lastSent.delete(tabId);
    }
    this.api.tabs.sendMessage(tabId, { type: 'ldm-track-video', on: this.follows(tabId) }).catch(() => {});
  }

  /**
   * The user turned the panel off or on in the tab: LDM hears the video is gone (so the panel
   * hides at once), or sees it again once the frames report.
   */
  panelSwitched(tabId) {
    if (!this.tracked.has(tabId)) {
      return;
    }
    if (this.isPanelOff(tabId)) {
      this.send(tabId).catch(() => {});
      this.frames.delete(tabId);
    }
    this.api.tabs.sendMessage(tabId, { type: 'ldm-track-video', on: this.follows(tabId) }).catch(() => {});
  }

  /** @private True when the frames of the tab should report its video. */
  follows(tabId) {
    return this.tracked.has(tabId) && !this.isPanelOff(tabId);
  }

  /** A frame's script started; it follows the video when its tab is tracked. */
  frameReady(sender) {
    const tabId = sender.tab?.id;
    if (this.follows(tabId)) {
      this.api.tabs.sendMessage(tabId, { type: 'ldm-track-video', on: true }, { frameId: sender.frameId ?? 0 })
        .catch(() => {});
    }
  }

  /** A frame reported its video. */
  onFrameReport(message, sender) {
    const tabId = sender.tab?.id;
    if (!this.follows(tabId)) {
      return;
    }
    const frameId = Number.isSafeInteger(sender.frameId) ? sender.frameId : 0;
    const report = cleanFrameReport(message, frameId === 0);
    if (!report) {
      return;
    }
    let frames = this.frames.get(tabId);
    if (!frames) {
      frames = new Map();
      this.frames.set(tabId, frames);
    }
    frames.set(frameId, { report, time: Date.now() });
    this.send(tabId).catch(() => {});
  }

  /** Something about tabs or windows changed: send every tracked tab again. */
  refreshAll() {
    for (const tabId of this.tracked) {
      this.send(tabId).catch(() => {});
    }
  }

  /** The tab closed or loads another page. */
  forget(tabId) {
    this.tracked.delete(tabId);
    this.frames.delete(tabId);
    this.lastSent.delete(tabId);
  }

  /** A new LDM connected: it has no media for any tab yet. */
  forgetAll() {
    for (const tabId of this.tracked) {
      this.api.tabs.sendMessage(tabId, { type: 'ldm-track-video', on: false }).catch(() => {});
    }
    this.tracked.clear();
    this.frames.clear();
    this.lastSent.clear();
  }

  /** @private */
  async send(tabId) {
    const { video, top } = chooseVideo(this.frames.get(tabId) ?? new Map(), Date.now());
    if (!top) {
      return;
    }
    const tab = await this.api.tabs.get(tabId).catch(() => null);
    const window = tab ? await this.api.windows.get(tab.windowId).catch(() => null) : null;
    if (!tab || !window) {
      return;
    }
    const position = makeVideoPosition({
      tabId,
      visible: Boolean(tab.active) && window.state !== 'minimized' && !top.hidden && video !== null &&
               !this.isPanelOff(tabId),
      fullscreen: top.fullscreen || window.state === 'fullscreen',
      window: { x: window.left ?? 0, y: window.top ?? 0, width: window.width ?? 0, height: window.height ?? 0 },
      viewport: top.viewport,
      viewportPositionKnown: top.viewportPositionKnown,
      video: video ?? { x: 0, y: 0, width: 0, height: 0 },
      pixelRatio: top.pixelRatio,
    });
    if (!position) {
      return;
    }
    const text = JSON.stringify({ ...position, id: '' });
    if (this.lastSent.get(tabId) === text) {
      return;
    }
    this.lastSent.set(tabId, text);
    this.connection.post(position);
  }
}

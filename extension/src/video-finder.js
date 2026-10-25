// Finds the video a page plays and reports where it is, so LDM can place its panel over it (M3
// design section 6). Runs in every frame, but stays idle until the background says LDM has media
// for this tab. Page code runs beside this script, so everything here is page-controlled; the
// background and LDM treat it only as a hint where to draw.
//
// A frame knows only its own viewport. Each frame therefore tells its child frames where they sit
// in the top frame's viewport (a message to each iframe's window), and reports its video in those
// coordinates.
(() => {
  const api = globalThis.browser ?? globalThis.chrome;
  if (globalThis.__ldmVideoFinder) {
    return;  // injected twice (on install and by the manifest)
  }
  globalThis.__ldmVideoFinder = true;

  const OFFSET_KEY = '__ldmFrameOffset';
  // Smaller videos are previews and icons, not something to download.
  const MIN_WIDTH = 160;
  const MIN_HEIGHT = 90;
  // A report goes out when something changed, and every few seconds even when nothing did, so the
  // background notices a frame that went away.
  const HEARTBEAT_MS = 3000;

  const isTop = window === window.top;
  let tracking = false;
  let offset = isTop ? { x: 0, y: 0 } : null;  // unknown until the parent frame says
  let lastReport = '';
  let lastReportTime = 0;

  /**
   * The page's videos. A player built as a web component keeps its video inside a shadow root
   * (VK's), so when the page itself has none, the open shadow roots are searched too.
   */
  function pageVideos() {
    const videos = [...document.querySelectorAll('video')];
    if (videos.length > 0) {
      return videos;
    }
    const search = (root) => {
      for (const element of root.querySelectorAll('*')) {
        if (element.shadowRoot) {
          videos.push(...element.shadowRoot.querySelectorAll('video'));
          search(element.shadowRoot);
        }
      }
    };
    search(document);
    return videos;
  }

  /** The largest visible video, playing ones first; null when there is none. */
  function bestVideo() {
    let best = null;
    let bestScore = 0;
    for (const video of pageVideos()) {
      const rect = video.getBoundingClientRect();
      const visibleWidth = Math.min(rect.right, window.innerWidth) - Math.max(rect.left, 0);
      const visibleHeight = Math.min(rect.bottom, window.innerHeight) - Math.max(rect.top, 0);
      if (rect.width < MIN_WIDTH || rect.height < MIN_HEIGHT || visibleWidth <= 0 || visibleHeight <= 0) {
        continue;
      }
      const playing = !video.paused && !video.ended && video.readyState > 2;
      const score = visibleWidth * visibleHeight * (playing ? 4 : 1);
      if (score > bestScore) {
        best = { rect, playing };
        bestScore = score;
      }
    }
    return best ? { ...best, score: bestScore } : null;
  }

  /** Tells each child frame where it sits in the top frame's viewport. */
  function tellChildFrames() {
    if (!offset) {
      return;
    }
    for (const frame of document.querySelectorAll('iframe, frame')) {
      const rect = frame.getBoundingClientRect();
      try {
        frame.contentWindow?.postMessage({ [OFFSET_KEY]: {
          x: offset.x + rect.left + frame.clientLeft,
          y: offset.y + rect.top + frame.clientTop,
        } }, '*');
      } catch {
        // a frame being torn down
      }
    }
  }

  function report() {
    if (!tracking) {
      return;
    }
    tellChildFrames();
    if (!offset) {
      return;
    }
    const found = bestVideo();
    const message = {
      type: 'video-rect',
      video: found ? {
        x: offset.x + found.rect.left, y: offset.y + found.rect.top, width: found.rect.width, height: found.rect.height,
      } : null,
      playing: found?.playing ?? false,
      score: found?.score ?? 0,
      hidden: document.visibilityState !== 'visible',
      fullscreen: Boolean(document.fullscreenElement),
    };
    if (isTop) {
      message.viewport = {
        x: Number.isFinite(window.mozInnerScreenX) ? window.mozInnerScreenX : 0,
        y: Number.isFinite(window.mozInnerScreenY) ? window.mozInnerScreenY : 0,
        width: window.innerWidth,
        height: window.innerHeight,
      };
      message.viewportPositionKnown = Number.isFinite(window.mozInnerScreenX);
      message.pixelRatio = window.devicePixelRatio;
    }
    const text = JSON.stringify(message);
    const now = Date.now();
    if (text === lastReport && now - lastReportTime < HEARTBEAT_MS) {
      return;
    }
    lastReport = text;
    lastReportTime = now;
    api.runtime.sendMessage(message).catch(() => {});
  }

  // Positions change with scrolling, resizing, layout and playback; checking once per display
  // frame while tracking catches all of them, including players that move without an event.
  // Browsers pause animation frames in hidden tabs; visibilitychange reports those.
  function onFrame() {
    if (!tracking) {
      return;
    }
    report();
    requestAnimationFrame(onFrame);
  }

  function setTracking(on) {
    const started = on && !tracking;
    tracking = on;
    lastReport = '';
    if (started) {
      requestAnimationFrame(onFrame);
    }
  }

  window.addEventListener('message', (event) => {
    // Only the parent frame may say where this frame is.
    if (isTop || event.source !== window.parent) {
      return;
    }
    const value = event.data?.[OFFSET_KEY];
    if (value && Number.isFinite(value.x) && Number.isFinite(value.y)) {
      offset = { x: value.x, y: value.y };
    }
  });
  document.addEventListener('visibilitychange', report);

  api.runtime.onMessage.addListener((message) => {
    if (message?.type === 'ldm-track-video' && typeof message.on === 'boolean') {
      setTracking(message.on);
    }
  });

  // A frame added or reloaded while the tab already has media asks whether to track.
  api.runtime.sendMessage({ type: 'video-finder-ready' }).catch(() => {});
})();

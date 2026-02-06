// LDM Content Script - Video Detection & Link Grabbing
// NON-INTRUSIVE VERSION - Does not modify video DOM

// ============================================
// SITE DETECTION
// ============================================

// Sites where we should NOT show the video overlay (DRM/anti-tampering)
// But the extension popup can still offer to download the page URL via yt-dlp
const SKIP_OVERLAY_SITES = [
    'netflix.com', 'hulu.com', 'disneyplus.com', 'disney.com',
    'primevideo.com', 'amazon.com/gp/video', 'amazon.co',
    'hbomax.com', 'max.com', 'peacocktv.com', 'paramountplus.com',
    'spotify.com',  // DRM protected - yt-dlp can't help
    'pornhub.com'   // Causes app freezing issues
];

// Sites that yt-dlp can download (we show "Download Page" option)
const YTDLP_SUPPORTED_SITES = [
    'youtube.com', 'youtu.be', 'youtube-nocookie.com',
    'vimeo.com', 'dailymotion.com',
    'twitter.com', 'x.com', 't.co',
    'facebook.com', 'fb.watch', 'instagram.com',
    'tiktok.com', 'vm.tiktok.com',
    'twitch.tv', 'clips.twitch.tv',
    'reddit.com', 'v.redd.it',
    'streamable.com', 'gfycat.com', 'imgur.com',
    'bilibili.com', 'nicovideo.jp',
    'soundcloud.com', 'bandcamp.com',
    'xvideos.com', 'xhamster.com',
    'crunchyroll.com', 'funimation.com',
    'ted.com', 'vk.com', 'ok.ru',
    'rumble.com', 'bitchute.com', 'odysee.com',
    'hotstar.com', 'zee5.com', 'sonyliv.com'
    // Note: pornhub removed due to causing app freezes
];

function shouldSkipOverlay() {
    const hostname = window.location.hostname;
    return SKIP_OVERLAY_SITES.some(site => hostname.includes(site));
}

function isYtDlpSupportedSite() {
    const hostname = window.location.hostname;
    return YTDLP_SUPPORTED_SITES.some(site => hostname.includes(site));
}

// ============================================
// URL VALIDATION
// ============================================

function isDownloadableUrl(url) {
    if (!url) return false;
    if (url.startsWith('blob:')) return false;
    if (url.startsWith('data:')) return false;
    if (url.includes('.m3u8')) return false;
    if (url.includes('.mpd')) return false;
    if (url.includes('manifest')) return false;
    return url.startsWith('http://') || url.startsWith('https://');
}

function getVideoSource(video) {
    if (video.src && isDownloadableUrl(video.src)) {
        return video.src;
    }
    const sources = video.querySelectorAll('source');
    for (const source of sources) {
        if (source.src && isDownloadableUrl(source.src)) {
            return source.src;
        }
    }
    if (video.currentSrc && isDownloadableUrl(video.currentSrc)) {
        return video.currentSrc;
    }
    return null;
}

// ============================================
// FLOATING OVERLAY (Non-intrusive)
// ============================================

// Only run overlay on non-protected sites AND non-yt-dlp sites
// (yt-dlp sites use the FAB button instead)
if (!shouldSkipOverlay() && !isYtDlpSupportedSite()) {
    let overlayContainer = null;
    let overlayShadow = null;  // Store reference to closed shadow root
    let currentVideo = null;
    let hideTimeout = null;
    let isOverButton = false;

    function createOverlay() {
        if (overlayContainer) return;

        overlayContainer = document.createElement('div');
        overlayContainer.id = 'ldm-floating-overlay';

        // Create shadow DOM to isolate styles (use 'open' so we can access it later)
        overlayShadow = overlayContainer.attachShadow({ mode: 'open' });

        overlayShadow.innerHTML = `
            <style>
                :host {
                    position: fixed !important;
                    z-index: 2147483647 !important;
                    pointer-events: none;
                    opacity: 0;
                    transition: opacity 0.15s ease;
                }
                :host(.visible) {
                    opacity: 1;
                    pointer-events: auto;
                }
                .ldm-btn {
                    display: flex;
                    align-items: center;
                    gap: 5px;
                    padding: 6px 10px;
                    background: rgba(99, 102, 241, 0.95);
                    color: #fff;
                    border: none;
                    border-radius: 6px;
                    font-family: system-ui, -apple-system, sans-serif;
                    font-size: 11px;
                    font-weight: 600;
                    cursor: pointer;
                    box-shadow: 0 2px 8px rgba(0,0,0,0.3);
                    transition: all 0.15s ease;
                    backdrop-filter: blur(4px);
                }
                .ldm-btn:hover {
                    background: rgba(129, 140, 248, 0.95);
                    transform: scale(1.03);
                }
                .ldm-btn.success {
                    background: rgba(16, 185, 129, 0.95);
                }
                .ldm-btn.error {
                    background: rgba(239, 68, 68, 0.95);
                }
                .ldm-btn.disabled {
                    background: rgba(107, 114, 128, 0.8);
                    cursor: default;
                }
            </style>
            <button class="ldm-btn" id="ldm-btn">⬇ LDM</button>
        `;

        const btn = overlayShadow.getElementById('ldm-btn');

        btn.addEventListener('click', (e) => {
            e.preventDefault();
            e.stopPropagation();
            handleDownload(btn);
        });

        btn.addEventListener('mouseenter', () => {
            isOverButton = true;
            if (hideTimeout) {
                clearTimeout(hideTimeout);
                hideTimeout = null;
            }
        });

        btn.addEventListener('mouseleave', () => {
            isOverButton = false;
            scheduleHide();
        });

        document.body.appendChild(overlayContainer);
    }

    function handleDownload(btn) {
        if (!currentVideo) return;

        const videoUrl = getVideoSource(currentVideo);

        if (!videoUrl) {
            btn.textContent = '⚠ No direct URL';
            btn.className = 'ldm-btn error';
            setTimeout(() => resetButton(btn), 2000);
            return;
        }

        btn.textContent = '⏳ Sending...';
        btn.className = 'ldm-btn';

        // Pass page URL as referer for protected downloads
        chrome.runtime.sendMessage({ action: 'downloadUrl', url: videoUrl, referer: window.location.href }, (response) => {
            if (chrome.runtime.lastError) {
                btn.textContent = '✗ Error';
                btn.className = 'ldm-btn error';
            } else if (response && response.success) {
                btn.textContent = '✓ Started!';
                btn.className = 'ldm-btn success';
            } else {
                btn.textContent = '✗ ' + (response?.error || 'Failed').substring(0, 10);
                btn.className = 'ldm-btn error';
            }
            setTimeout(() => resetButton(btn), 2000);
        });
    }

    function resetButton(btn) {
        if (currentVideo && getVideoSource(currentVideo)) {
            btn.textContent = '⬇ LDM';
            btn.className = 'ldm-btn';
        } else {
            btn.textContent = '⚠ Stream';
            btn.className = 'ldm-btn disabled';
        }
    }

    function showOverlay(video) {
        if (!overlayContainer) createOverlay();
        if (!overlayContainer || !overlayShadow) return;

        // Don't show overlay if no downloadable URL
        const hasDirectUrl = getVideoSource(video);
        if (!hasDirectUrl) {
            return;
        }

        currentVideo = video;
        const rect = video.getBoundingClientRect();

        // Position top-right of video, but ensure it stays within viewport
        const buttonWidth = 75;
        const buttonHeight = 30;
        const margin = 8;

        let top = rect.top + margin;
        let left = rect.right - buttonWidth - margin;

        // Keep within viewport
        if (top < 5) top = 5;
        if (left < 5) left = 5;
        if (left + buttonWidth > window.innerWidth - 5) {
            left = window.innerWidth - buttonWidth - 5;
        }

        overlayContainer.style.top = top + 'px';
        overlayContainer.style.left = left + 'px';
        overlayContainer.classList.add('visible');

        // Update button state
        const btn = overlayShadow.getElementById('ldm-btn');
        if (!btn) return;

        btn.textContent = '⬇ LDM';
        btn.className = 'ldm-btn';

        if (hideTimeout) {
            clearTimeout(hideTimeout);
            hideTimeout = null;
        }
    }

    function scheduleHide() {
        if (hideTimeout) clearTimeout(hideTimeout);
        hideTimeout = setTimeout(() => {
            if (!isOverButton && overlayContainer) {
                overlayContainer.classList.remove('visible');
                currentVideo = null;
            }
        }, 200);
    }

    // Event delegation - no DOM modification on videos
    document.addEventListener('mouseenter', (e) => {
        if (e.target.tagName !== 'VIDEO') return;

        const video = e.target;
        const rect = video.getBoundingClientRect();

        // Skip small videos
        if (rect.width < 200 || rect.height < 100) return;

        showOverlay(video);
    }, true);

    document.addEventListener('mouseleave', (e) => {
        if (e.target.tagName !== 'VIDEO') return;

        // Don't hide if moving to overlay (check by element reference, not id)
        const related = e.relatedTarget;
        if (related && (related === overlayContainer || overlayContainer?.contains(related))) return;

        scheduleHide();
    }, true);

    // Update position on scroll
    let ticking = false;
    window.addEventListener('scroll', () => {
        if (!currentVideo || !overlayContainer) return;
        if (!ticking) {
            requestAnimationFrame(() => {
                if (currentVideo) {
                    const rect = currentVideo.getBoundingClientRect();
                    const buttonWidth = 75;
                    const margin = 8;

                    let top = rect.top + margin;
                    let left = rect.right - buttonWidth - margin;

                    // Keep within viewport
                    if (top < 5) top = 5;
                    if (left < 5) left = 5;
                    if (left + buttonWidth > window.innerWidth - 5) {
                        left = window.innerWidth - buttonWidth - 5;
                    }

                    overlayContainer.style.top = top + 'px';
                    overlayContainer.style.left = left + 'px';
                }
                ticking = false;
            });
            ticking = true;
        }
    }, { passive: true });

    console.log('LDM: Video overlay enabled');
} else {
    console.log('LDM: Skipping overlay on protected site');
}

// ============================================
// PAGE DOWNLOAD BUTTON (for yt-dlp sites)
// ============================================

// Show floating download button on video sites (YouTube, Twitter, etc.)
if (isYtDlpSupportedSite()) {
    let pageDownloadBtn = null;
    let isMinimized = false;

    function createPageDownloadButton() {
        if (pageDownloadBtn) return;

        pageDownloadBtn = document.createElement('div');
        pageDownloadBtn.id = 'ldm-page-download';

        const shadow = pageDownloadBtn.attachShadow({ mode: 'closed' });

        shadow.innerHTML = `
            <style>
                :host {
                    position: fixed !important;
                    bottom: 80px;
                    right: 20px;
                    z-index: 2147483647 !important;
                    font-family: system-ui, -apple-system, sans-serif;
                }
                .ldm-fab {
                    display: flex;
                    align-items: center;
                    gap: 8px;
                    padding: 10px 14px;
                    background: linear-gradient(135deg, #6366f1 0%, #4f46e5 100%);
                    color: #fff;
                    border: none;
                    border-radius: 50px;
                    font-size: 12px;
                    font-weight: 600;
                    cursor: pointer;
                    box-shadow: 0 4px 15px rgba(99, 102, 241, 0.4);
                    transition: all 0.2s ease;
                    white-space: nowrap;
                }
                .ldm-fab:hover {
                    transform: translateY(-2px);
                    box-shadow: 0 6px 20px rgba(99, 102, 241, 0.5);
                }
                .ldm-fab.success {
                    background: linear-gradient(135deg, #10b981 0%, #059669 100%);
                    box-shadow: 0 4px 15px rgba(16, 185, 129, 0.4);
                }
                .ldm-fab.error {
                    background: linear-gradient(135deg, #ef4444 0%, #dc2626 100%);
                    box-shadow: 0 4px 15px rgba(239, 68, 68, 0.4);
                }
                .ldm-fab.minimized {
                    padding: 10px;
                    border-radius: 50%;
                }
                .ldm-fab.minimized .ldm-text {
                    display: none;
                }
                .ldm-icon {
                    font-size: 16px;
                }
                .ldm-close {
                    position: absolute;
                    top: -6px;
                    right: -6px;
                    width: 18px;
                    height: 18px;
                    background: #64748b;
                    border: none;
                    border-radius: 50%;
                    color: #fff;
                    font-size: 11px;
                    cursor: pointer;
                    display: flex;
                    align-items: center;
                    justify-content: center;
                    opacity: 0;
                    transition: opacity 0.2s;
                }
                .ldm-container:hover .ldm-close {
                    opacity: 1;
                }
                .ldm-container {
                    position: relative;
                }
            </style>
            <div class="ldm-container">
                <button class="ldm-fab" id="ldm-fab">
                    <span class="ldm-icon">⬇</span>
                    <span class="ldm-text">LDM Download</span>
                </button>
                <button class="ldm-close" id="ldm-close" title="Hide">×</button>
            </div>
        `;

        const fab = shadow.getElementById('ldm-fab');
        const closeBtn = shadow.getElementById('ldm-close');

        fab.addEventListener('click', () => {
            const pageUrl = window.location.href;
            fab.innerHTML = '<span class="ldm-icon">⏳</span><span class="ldm-text">Sending...</span>';

            // Pass page URL as both download URL and referer for yt-dlp sites
            chrome.runtime.sendMessage({ action: 'downloadUrl', url: pageUrl, referer: pageUrl }, (response) => {
                const icon = fab.querySelector('.ldm-icon');
                const text = fab.querySelector('.ldm-text');

                if (chrome.runtime.lastError) {
                    if (icon) icon.textContent = '✗';
                    if (text) text.textContent = 'Error';
                    fab.className = 'ldm-fab error';
                } else if (response && response.success) {
                    if (icon) icon.textContent = '✓';
                    if (text) text.textContent = 'Started!';
                    fab.className = 'ldm-fab success';
                } else {
                    if (icon) icon.textContent = '✗';
                    // Sanitize error message - only show first 20 chars
                    const errorMsg = String(response?.error || 'Failed').substring(0, 20);
                    if (text) text.textContent = errorMsg;
                    fab.className = 'ldm-fab error';
                }

                setTimeout(() => {
                    fab.innerHTML = '<span class="ldm-icon">⬇</span><span class="ldm-text">Download Video</span>';
                    fab.className = 'ldm-fab';
                }, 2500);
            });
        });

        closeBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            pageDownloadBtn.remove();
            // Remember preference for this session
            sessionStorage.setItem('ldm-hidden', 'true');
        });

        // Don't show if user closed it this session
        if (sessionStorage.getItem('ldm-hidden') === 'true') {
            return;
        }

        document.body.appendChild(pageDownloadBtn);
    }

    // Wait for page to load
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', () => {
            setTimeout(createPageDownloadButton, 1000);
        });
    } else {
        setTimeout(createPageDownloadButton, 1000);
    }

    console.log('LDM: Page download button enabled for yt-dlp site');
}

// ============================================
// MEDIA FILE EXTENSIONS
// ============================================

const MEDIA_EXTENSIONS = [
    'mp4', 'mkv', 'avi', 'mov', 'wmv', 'flv', 'webm', 'm4v', 'mpeg', 'mpg', '3gp',
    'mp3', 'wav', 'flac', 'aac', 'ogg', 'm4a', 'wma',
    'zip', 'rar', '7z', 'tar', 'gz', 'bz2',
    'pdf', 'doc', 'docx', 'xls', 'xlsx', 'ppt', 'pptx',
    'psd', 'ai', 'eps',
    'exe', 'msi', 'dmg', 'pkg', 'deb', 'rpm', 'apk'
];

// ============================================
// MESSAGE HANDLING (for popup)
// ============================================

chrome.runtime.onMessage.addListener(function(request, sender, sendResponse) {
    if (request.action === "scanVideos") {
        const videos = scanForVideos();
        sendResponse({ videos: videos });
    } else if (request.action === "grabLinks") {
        const links = grabLinks(request.mediaOnly);
        sendResponse({ links: links });
    } else if (request.action === "getPageUrl") {
        sendResponse({ url: window.location.href });
    }
    return true;
});

// ============================================
// VIDEO SCANNING (for popup Videos tab)
// ============================================

// Sites to completely skip video scanning (cause issues)
const SKIP_VIDEO_SCAN_SITES = ['pornhub.com'];

function shouldSkipVideoScan() {
    const hostname = window.location.hostname;
    return SKIP_VIDEO_SCAN_SITES.some(site => hostname.includes(site));
}

function scanForVideos() {
    const videos = [];

    // Skip scanning entirely for problematic sites
    if (shouldSkipVideoScan()) {
        return videos;
    }

    const seen = new Set();

    // If this is a yt-dlp supported site, offer to download the page URL
    if (isYtDlpSupportedSite()) {
        const pageUrl = window.location.href;
        videos.push({
            url: pageUrl,
            title: document.title || 'Video Page',
            quality: 'yt-dlp',
            type: 'page'
        });
    }

    // HTML5 Video elements - only downloadable URLs
    document.querySelectorAll('video').forEach(video => {
        if (video.src && !seen.has(video.src) && isDownloadableUrl(video.src)) {
            seen.add(video.src);
            videos.push({
                url: video.src,
                title: getVideoTitle(video),
                quality: getVideoQuality(video),
                type: 'video'
            });
        }

        video.querySelectorAll('source').forEach(source => {
            if (source.src && !seen.has(source.src) && isDownloadableUrl(source.src)) {
                seen.add(source.src);
                videos.push({
                    url: source.src,
                    title: getVideoTitle(video),
                    quality: source.type || 'Unknown',
                    type: 'video'
                });
            }
        });
    });

    // Audio elements
    document.querySelectorAll('audio').forEach(audio => {
        if (audio.src && !seen.has(audio.src) && isDownloadableUrl(audio.src)) {
            seen.add(audio.src);
            videos.push({
                url: audio.src,
                title: document.title,
                quality: 'Audio',
                type: 'audio'
            });
        }

        audio.querySelectorAll('source').forEach(source => {
            if (source.src && !seen.has(source.src) && isDownloadableUrl(source.src)) {
                seen.add(source.src);
                videos.push({
                    url: source.src,
                    title: document.title,
                    quality: source.type || 'Audio',
                    type: 'audio'
                });
            }
        });
    });

    // Links to video/audio files
    document.querySelectorAll('a[href]').forEach(link => {
        const href = link.href;
        if (isVideoUrl(href) && !seen.has(href) && isDownloadableUrl(href)) {
            seen.add(href);
            videos.push({
                url: href,
                title: link.textContent.trim() || getFilenameFromUrl(href),
                quality: getExtension(href).toUpperCase(),
                type: 'link'
            });
        }
    });

    // NOTE: YouTube/Vimeo embeds are intentionally NOT included
    // because they cannot be downloaded directly by LDM

    return videos;
}

// ============================================
// LINK GRABBING
// ============================================

function grabLinks(mediaOnly) {
    const links = [];
    const seen = new Set();

    document.querySelectorAll('a[href]').forEach(link => {
        const href = link.href;

        if (!href || href.startsWith('javascript:') || href.startsWith('#') || href.startsWith('mailto:')) {
            return;
        }

        if (seen.has(href)) return;
        seen.add(href);

        if (mediaOnly) {
            const ext = getExtension(href).toLowerCase();
            if (!MEDIA_EXTENSIONS.includes(ext)) {
                return;
            }
        }

        links.push(href);
    });

    // Also grab video/audio sources (filter out blob URLs)
    if (mediaOnly) {
        document.querySelectorAll('video source, audio source, video[src], audio[src]').forEach(el => {
            const src = el.src;
            if (src && !seen.has(src) && isDownloadableUrl(src)) {
                seen.add(src);
                links.push(src);
            }
        });
    }

    return links;
}

// ============================================
// HELPER FUNCTIONS
// ============================================

function getVideoTitle(video) {
    if (video.title) return video.title;
    if (video.alt) return video.alt;

    const figure = video.closest('figure');
    if (figure) {
        const caption = figure.querySelector('figcaption');
        if (caption) return caption.textContent.trim();
    }

    if (video.getAttribute('aria-label')) return video.getAttribute('aria-label');

    return document.title;
}

function getVideoQuality(video) {
    if (video.videoWidth && video.videoHeight) {
        const height = video.videoHeight;
        if (height >= 2160) return '4K';
        if (height >= 1440) return '1440p';
        if (height >= 1080) return '1080p';
        if (height >= 720) return '720p';
        if (height >= 480) return '480p';
        return height + 'p';
    }
    return 'Unknown';
}

function isVideoUrl(url) {
    const videoExtensions = ['mp4', 'mkv', 'avi', 'mov', 'wmv', 'flv', 'webm', 'm4v', 'mpeg', 'mpg', '3gp', 'mp3', 'wav', 'flac', 'aac', 'ogg', 'm4a'];
    const ext = getExtension(url).toLowerCase();
    return videoExtensions.includes(ext);
}

function getExtension(url) {
    try {
        const pathname = new URL(url).pathname;
        const filename = pathname.split('/').pop();
        const ext = filename.split('.').pop();
        return ext.split('?')[0];
    } catch (e) {
        return '';
    }
}

function getFilenameFromUrl(url) {
    try {
        const pathname = new URL(url).pathname;
        return decodeURIComponent(pathname.split('/').pop()) || 'download';
    } catch (e) {
        return 'download';
    }
}

console.log('LDM Content Script loaded');

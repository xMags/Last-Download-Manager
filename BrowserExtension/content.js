// LDM Content Script - Video Detection & Link Grabbing
// NON-INTRUSIVE VERSION - Does not modify video DOM

// ============================================
// SITE DETECTION
// ============================================

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

// Floating/hover page UI is intentionally disabled.
// Extension actions are available from popup/context menu only.

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

// LDM Browser Extension v3.0 - Background Service Worker
// Full-featured download manager integration

const LDM_URL = "http://127.0.0.1:45678";

// ============================================
// STATE
// ============================================

let settings = {
    interceptAll: true,
    ignoredExtensions: ['jpg', 'jpeg', 'png', 'gif', 'bmp', 'webp'],
    minFileSize: 0, // KB, 0 = no minimum
    domainWhitelist: [],
    domainBlacklist: [],
    notificationsEnabled: true,
    soundEnabled: true
};

let authToken = null;
let isConnected = false;

// ============================================
// INITIALIZATION
// ============================================

// Load settings on startup
chrome.storage.sync.get({
    interceptAll: true,
    ignoredExtensions: ['jpg', 'jpeg', 'png', 'gif', 'bmp', 'webp'],
    minFileSize: 0,
    domainWhitelist: [],
    domainBlacklist: [],
    notificationsEnabled: true,
    soundEnabled: true
}, function(items) {
    settings = items;
});

// Load auth token
chrome.storage.local.get(['ldmAuthToken'], function(result) {
    if (result.ldmAuthToken) {
        authToken = result.ldmAuthToken;
    } else {
        fetchAuthToken();
    }
});

// Initial connection check
checkConnection();

// Periodic connection check (every 30 seconds)
setInterval(() => {
    checkConnection();
}, 30000);

// ============================================
// AUTH & CONNECTION
// ============================================

async function fetchAuthToken() {
    try {
        const response = await fetch(LDM_URL + "/token", { method: "GET", mode: "cors" });
        if (response.ok) {
            const data = await response.json();
            authToken = data.token;
            chrome.storage.local.set({ ldmAuthToken: authToken });
            return true;
        }
    } catch (e) {
        // LDM not running
    }
    return false;
}

async function checkConnection() {
    try {
        const response = await fetch(LDM_URL + "/ping", { method: "GET", mode: "cors" });
        if (response.ok) {
            isConnected = true;
            updateBadge(true);

            // Fetch token if we don't have one
            if (!authToken) {
                await fetchAuthToken();
            }
            return true;
        }
    } catch (e) {
        // Connection failed
    }

    isConnected = false;
    updateBadge(false);
    return false;
}

// ============================================
// BADGE MANAGEMENT
// ============================================

function updateBadge(connected, downloadCount = null) {
    if (downloadCount !== null && downloadCount > 0) {
        chrome.action.setBadgeText({ text: downloadCount.toString() });
        chrome.action.setBadgeBackgroundColor({ color: '#6366f1' });
    } else if (connected) {
        chrome.action.setBadgeText({ text: '' });
        chrome.action.setBadgeBackgroundColor({ color: '#10b981' });
    } else {
        chrome.action.setBadgeText({ text: '!' });
        chrome.action.setBadgeBackgroundColor({ color: '#ef4444' });
    }
}

// ============================================
// DOWNLOAD FUNCTIONS
// ============================================

async function sendToLDM(url, showNotification = true, referer = null) {
    try {
        if (!authToken) {
            await fetchAuthToken();
        }

        // Build request body with optional referer
        const requestBody = { url: url };
        if (referer) {
            requestBody.referer = referer;
        }

        const response = await fetch(LDM_URL + "/download", {
            method: "POST",
            mode: "cors",
            headers: {
                "Content-Type": "application/json",
                "X-Auth-Token": authToken || ""
            },
            body: JSON.stringify(requestBody)
        });

        if (response.ok) {
            const data = await response.json();
            console.log("LDM Response:", data);

            // Update stats
            updateStats(true);

            // Add to recent
            addToRecent(url, true);

            // Show notification
            if (showNotification && settings.notificationsEnabled) {
                showDownloadNotification("Download Started", getFilenameFromUrl(url));
            }

            return { success: true, data: data };
        } else if (response.status === 401) {
            // Token stale, refresh and retry
            console.log("Auth failed, refreshing token...");
            authToken = null;
            if (await fetchAuthToken()) {
                const retryResponse = await fetch(LDM_URL + "/download", {
                    method: "POST",
                    mode: "cors",
                    headers: {
                        "Content-Type": "application/json",
                        "X-Auth-Token": authToken || ""
                    },
                    body: JSON.stringify(requestBody)  // Use original body with referer
                });
                if (retryResponse.ok) {
                    updateStats(true);
                    addToRecent(url, true);
                    if (showNotification && settings.notificationsEnabled) {
                        showDownloadNotification("Download Started", getFilenameFromUrl(url));
                    }
                    return { success: true, data: await retryResponse.json() };
                }
            }
            return { success: false, error: "Authentication failed" };
        } else {
            addToRecent(url, false);
            return { success: false, error: "HTTP " + response.status };
        }
    } catch (e) {
        console.error("LDM Connection Error:", e.message);
        addToRecent(url, false);
        return { success: false, error: "LDM not running" };
    }
}

// ============================================
// NOTIFICATIONS
// ============================================

function showDownloadNotification(title, message) {
    chrome.notifications.create({
        type: 'basic',
        iconUrl: 'icons/icon128.png',
        title: title,
        message: message,
        silent: !settings.soundEnabled
    });
}

// ============================================
// STATS & HISTORY
// ============================================

function updateStats(success) {
    chrome.storage.local.get(['stats'], function(result) {
        const stats = result.stats || { totalDownloads: 0, totalBytes: 0, todayDownloads: 0, lastDate: null };
        const today = new Date().toDateString();

        if (stats.lastDate !== today) {
            stats.todayDownloads = 0;
            stats.lastDate = today;
        }

        if (success) {
            stats.totalDownloads++;
            stats.todayDownloads++;
        }

        chrome.storage.local.set({ stats: stats });
    });
}

function addToRecent(url, success) {
    chrome.storage.local.get(['recentDownloads'], function(result) {
        const recent = result.recentDownloads || [];
        recent.unshift({
            url: url,
            timestamp: Date.now(),
            success: success
        });
        chrome.storage.local.set({ recentDownloads: recent.slice(0, 50) });
    });
}

// ============================================
// FILTERING
// ============================================

function isIgnoredExtension(url) {
    if (!url) return false;

    const cleanUrl = url.split('?')[0].split('#')[0];
    const dotIndex = cleanUrl.lastIndexOf('.');
    if (dotIndex === -1) return false;

    const ext = cleanUrl.substring(dotIndex + 1).toLowerCase();
    return settings.ignoredExtensions.includes(ext);
}

function isDomainAllowed(url) {
    try {
        const hostname = new URL(url).hostname;

        // Check blacklist first
        if (settings.domainBlacklist && settings.domainBlacklist.length > 0) {
            for (const domain of settings.domainBlacklist) {
                if (hostname.includes(domain)) return false;
            }
        }

        // Check whitelist (if set, only allow these)
        if (settings.domainWhitelist && settings.domainWhitelist.length > 0) {
            for (const domain of settings.domainWhitelist) {
                if (hostname.includes(domain)) return true;
            }
            return false;
        }

        return true;
    } catch (e) {
        return true;
    }
}

// ============================================
// UTILITY FUNCTIONS
// ============================================

function getFilenameFromUrl(url) {
    try {
        const pathname = new URL(url).pathname;
        const filename = pathname.split('/').pop() || 'download';
        return decodeURIComponent(filename).substring(0, 50);
    } catch (e) {
        return 'download';
    }
}

// ============================================
// MESSAGE HANDLING
// ============================================

chrome.runtime.onMessage.addListener(function(request, sender, sendResponse) {
    if (request.action === "updateSettings") {
        chrome.storage.sync.get({
            interceptAll: true,
            ignoredExtensions: ['jpg', 'jpeg', 'png', 'gif', 'bmp', 'webp'],
            minFileSize: 0,
            domainWhitelist: [],
            domainBlacklist: [],
            notificationsEnabled: true,
            soundEnabled: true
        }, function(items) {
            settings = items;
        });

        chrome.storage.local.get(['ldmAuthToken'], function(result) {
            if (result.ldmAuthToken) {
                authToken = result.ldmAuthToken;
            }
        });
    }

    if (request.action === "checkConnection") {
        checkConnection().then(connected => {
            sendResponse({ connected: connected });
        });
        return true;
    }

    if (request.action === "updateBadge") {
        updateBadge(request.connected);
    }

    if (request.action === "showNotification") {
        if (settings.notificationsEnabled) {
            showDownloadNotification(request.title, request.message);
        }
    }

    if (request.action === "downloadUrl") {
        sendToLDM(request.url, true, request.referer || null).then(sendResponse);
        return true;
    }
});

// ============================================
// CONTEXT MENU
// ============================================

chrome.runtime.onInstalled.addListener(function() {
    // Create context menus
    chrome.contextMenus.create({
        "id": "downloadWithLDM",
        "title": "Download with LDM",
        "contexts": ["link", "image", "video", "audio"]
    });

    chrome.contextMenus.create({
        "id": "downloadAllLinks",
        "title": "Download All Links with LDM",
        "contexts": ["page"]
    });

    chrome.contextMenus.create({
        "id": "downloadAllMedia",
        "title": "Download All Media with LDM",
        "contexts": ["page"]
    });

    // Fetch auth token
    fetchAuthToken();

    // Check connection
    checkConnection();
});

chrome.contextMenus.onClicked.addListener(function(info, tab) {
    if (info.menuItemId === "downloadWithLDM") {
        const url = info.linkUrl || info.srcUrl;
        if (url) {
            // Pass the page URL as referer for protected downloads
            const referer = tab ? tab.url : null;
            sendToLDM(url, true, referer);
        }
    } else if (info.menuItemId === "downloadAllLinks" || info.menuItemId === "downloadAllMedia") {
        const mediaOnly = info.menuItemId === "downloadAllMedia";
        const referer = tab ? tab.url : null;

        chrome.tabs.sendMessage(tab.id, {
            action: "grabLinks",
            mediaOnly: mediaOnly
        }, async function(response) {
            if (response && response.links && response.links.length > 0) {
                let count = 0;
                for (const url of response.links) {
                    const result = await sendToLDM(url, false, referer);
                    if (result.success) count++;
                }

                if (settings.notificationsEnabled) {
                    showDownloadNotification(
                        "Batch Download",
                        `Started ${count} of ${response.links.length} downloads`
                    );
                }
            }
        });
    }
});

// ============================================
// KEYBOARD SHORTCUTS
// ============================================

chrome.commands.onCommand.addListener(function(command) {
    if (command === "download-current") {
        // Get current tab URL
        chrome.tabs.query({ active: true, currentWindow: true }, function(tabs) {
            if (tabs[0]) {
                // For page downloads, the page is both the URL and the referer
                sendToLDM(tabs[0].url, true, tabs[0].url);
            }
        });
    } else if (command === "grab-all-links") {
        // Grab all media links from current page
        chrome.tabs.query({ active: true, currentWindow: true }, function(tabs) {
            if (tabs[0]) {
                const pageReferer = tabs[0].url;
                chrome.tabs.sendMessage(tabs[0].id, {
                    action: "grabLinks",
                    mediaOnly: true
                }, async function(response) {
                    if (response && response.links && response.links.length > 0) {
                        let count = 0;
                        for (const url of response.links) {
                            const result = await sendToLDM(url, false, pageReferer);
                            if (result.success) count++;
                        }

                        if (settings.notificationsEnabled) {
                            showDownloadNotification(
                                "Media Grabbed",
                                `Started ${count} downloads`
                            );
                        }
                    } else {
                        showDownloadNotification(
                            "No Media Found",
                            "No downloadable media on this page"
                        );
                    }
                });
            }
        });
    }
});

// ============================================
// DOWNLOAD INTERCEPTION
// ============================================

chrome.downloads.onCreated.addListener(async function(downloadItem) {
    if (downloadItem.state !== "in_progress") return;

    // Check if interception is enabled
    if (!settings.interceptAll) return;

    // Skip blob and data URLs - LDM can't handle these
    if (downloadItem.url.startsWith('blob:') || downloadItem.url.startsWith('data:')) {
        console.log("LDM: Ignoring blob/data URL");
        return;
    }

    // Skip streaming URLs
    if (downloadItem.url.includes('.m3u8') || downloadItem.url.includes('.mpd')) {
        console.log("LDM: Ignoring streaming URL");
        return;
    }

    // Check extension filter
    if (isIgnoredExtension(downloadItem.url)) {
        console.log("LDM: Ignoring due to extension:", downloadItem.url);
        return;
    }

    // Check domain filter
    if (!isDomainAllowed(downloadItem.url)) {
        console.log("LDM: Ignoring due to domain:", downloadItem.url);
        return;
    }

    // Check file size (if available)
    if (settings.minFileSize > 0 && downloadItem.fileSize) {
        const sizeKB = downloadItem.fileSize / 1024;
        if (sizeKB < settings.minFileSize) {
            console.log("LDM: Ignoring due to size:", sizeKB, "KB");
            return;
        }
    }

    // Get the referer from the download item or active tab
    let referer = downloadItem.referrer || null;
    if (!referer) {
        // Try to get from active tab
        try {
            const tabs = await chrome.tabs.query({ active: true, currentWindow: true });
            if (tabs[0]) {
                referer = tabs[0].url;
            }
        } catch (e) {
            // Ignore error
        }
    }

    // Send to LDM with referer
    const result = await sendToLDM(downloadItem.url, true, referer);

    if (result.success) {
        // Cancel Chrome's download
        chrome.downloads.cancel(downloadItem.id, function() {
            chrome.downloads.erase({ id: downloadItem.id });
        });
    }
    // If LDM fails, let Chrome continue
});

// ============================================
// PERIODIC TASKS
// ============================================

// Refresh auth token every 5 minutes
setInterval(async function() {
    if (isConnected && !authToken) {
        await fetchAuthToken();
    }
}, 300000);

console.log("LDM Background Service Worker loaded");

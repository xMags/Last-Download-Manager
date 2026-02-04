// LDM Browser Extension - HTTP-based integration
// Communicates with LDM via local HTTP server on port 45678

const LDM_URL = "http://127.0.0.1:45678";

// Default Settings
var settings = {
    interceptAll: true,
    ignoredExtensions: ['jpg', 'jpeg', 'png', 'gif', 'bmp', 'webp']
};

// Load settings on startup
chrome.storage.sync.get({
    interceptAll: true,
    ignoredExtensions: ['jpg', 'jpeg', 'png', 'gif', 'bmp', 'webp']
}, function(items) {
    settings = items;
});

// Listen for settings changes
chrome.runtime.onMessage.addListener(function(request, sender, sendResponse) {
    if (request.action === "updateSettings") {
        chrome.storage.sync.get({
            interceptAll: true,
            ignoredExtensions: ['jpg', 'jpeg', 'png', 'gif', 'bmp', 'webp']
        }, function(items) {
            settings = items;
        });
    }
    if (request.action === "checkConnection") {
        checkLDMConnection().then(sendResponse);
        return true; // Keep channel open for async response
    }
});

// Helper: Check if extension is ignored
function isIgnoredExtension(url) {
    if (!url) return false;
    
    // Extract extension (simple check)
    var cleanUrl = url.split('?')[0].split('#')[0];
    var dotIndex = cleanUrl.lastIndexOf('.');
    if (dotIndex === -1) return false;
    
    var ext = cleanUrl.substring(dotIndex + 1).toLowerCase();
    
    return settings.ignoredExtensions.includes(ext);
}

// Check if LDM is running
async function checkLDMConnection() {
    try {
        const response = await fetch(LDM_URL + "/ping", {
            method: "GET",
            mode: "cors"
        });
        if (response.ok) {
            const data = await response.json();
            return { connected: true, app: data.app, version: data.version };
        }
    } catch (e) {
        // Connection failed
    }
    return { connected: false };
}

// Send URL to LDM
async function sendToLDM(url) {
    try {
        const response = await fetch(LDM_URL + "/download", {
            method: "POST",
            mode: "cors",
            headers: {
                "Content-Type": "application/json"
            },
            body: JSON.stringify({ url: url })
        });
        
        if (response.ok) {
            const data = await response.json();
            console.log("LDM Response:", data);
            return { success: true, data: data };
        } else {
            console.error("LDM Error: HTTP " + response.status);
            return { success: false, error: "HTTP " + response.status };
        }
    } catch (e) {
        console.error("LDM Connection Error:", e.message);
        return { success: false, error: "LDM not running or connection refused" };
    }
}

// Create Context Menu
chrome.runtime.onInstalled.addListener(function() {
    chrome.contextMenus.create({
        "id": "downloadWithLDM",
        "title": "Download with LDM",
        "contexts": ["link", "image", "video", "audio"]
    });
});

// Handle Context Menu Clicks
chrome.contextMenus.onClicked.addListener(function(info, tab) {
    if (info.menuItemId === "downloadWithLDM") {
        var url = info.linkUrl || info.srcUrl;
        if (url) {
            sendToLDM(url);
        }
    }
});

// Listen for new downloads
chrome.downloads.onCreated.addListener(async function(downloadItem) {
    if (downloadItem.state !== "in_progress") return;
    
    // Check if we should intercept
    if (!settings.interceptAll) return;
    
    // Check if extension is ignored
    if (isIgnoredExtension(downloadItem.url)) {
        console.log("LDM: Ignoring download due to file extension:", downloadItem.url);
        return;
    }
    
    // Try to send to LDM
    const result = await sendToLDM(downloadItem.url);
    
    if (result.success) {
        // Cancel Chrome's download since LDM will handle it
        chrome.downloads.cancel(downloadItem.id, function() {
            chrome.downloads.erase({id: downloadItem.id});
        });
    }
    // If LDM is not running, let Chrome continue the download normally
});

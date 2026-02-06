const LDM_URL = "http://127.0.0.1:45678";
let authToken = null;
let isConnected = false;
let statusInterval = null;

// ============================================
// INITIALIZATION
// ============================================

document.addEventListener('DOMContentLoaded', async function() {
    // Load theme
    loadTheme();

    // Load settings
    loadSettings();

    // Check connection
    await checkConnection();

    // Load stats
    loadStats();

    // Load recent downloads
    loadRecentDownloads();

    // Start fetching active downloads status
    startStatusPolling();

    // Setup all event listeners
    setupEventListeners();

    // Focus input
    document.getElementById('manualUrl').focus();
});

// Cleanup on popup close
window.addEventListener('unload', function() {
    if (statusInterval) {
        clearInterval(statusInterval);
    }
});

// ============================================
// THEME MANAGEMENT
// ============================================

function loadTheme() {
    chrome.storage.local.get(['darkMode', 'colorTheme'], function(result) {
        // Apply dark mode
        const isDark = result.darkMode || window.matchMedia('(prefers-color-scheme: dark)').matches;
        if (isDark) {
            document.documentElement.classList.add('dark');
        }
        updateThemeIcon();

        // Apply color theme
        if (result.colorTheme) {
            setColorTheme(result.colorTheme);
        }
    });
}

function toggleTheme() {
    const isDark = document.documentElement.classList.toggle('dark');
    chrome.storage.local.set({ darkMode: isDark });
    updateThemeIcon();
    playSound('click');
}

function updateThemeIcon() {
    const btn = document.getElementById('themeToggle');
    const isDark = document.documentElement.classList.contains('dark');
    btn.innerHTML = isDark ? '&#9728;' : '&#127769;';
}

function setColorTheme(theme) {
    const themes = {
        indigo: { primary: '#6366f1', dark: '#4f46e5', light: '#818cf8' },
        blue: { primary: '#3b82f6', dark: '#2563eb', light: '#60a5fa' },
        green: { primary: '#10b981', dark: '#059669', light: '#34d399' },
        purple: { primary: '#8b5cf6', dark: '#7c3aed', light: '#a78bfa' },
        pink: { primary: '#ec4899', dark: '#db2777', light: '#f472b6' },
        orange: { primary: '#f97316', dark: '#ea580c', light: '#fb923c' },
        red: { primary: '#ef4444', dark: '#dc2626', light: '#f87171' },
        gray: { primary: '#6b7280', dark: '#4b5563', light: '#9ca3af' }
    };

    const colors = themes[theme] || themes.indigo;
    document.documentElement.style.setProperty('--primary', colors.primary);
    document.documentElement.style.setProperty('--primary-dark', colors.dark);
    document.documentElement.style.setProperty('--primary-light', colors.light);
}

// ============================================
// CONNECTION & AUTH
// ============================================

async function checkConnection() {
    const statusEl = document.getElementById('connectionStatus');
    const btn = document.getElementById('downloadBtn');
    const offlineNotice = document.getElementById('offlineNotice');

    try {
        const pingResponse = await fetch(LDM_URL + "/ping", { method: "GET", mode: "cors" });
        if (pingResponse.ok) {
            const data = await pingResponse.json();

            // Fetch auth token
            try {
                const tokenResponse = await fetch(LDM_URL + "/token", { method: "GET", mode: "cors" });
                if (tokenResponse.ok) {
                    const tokenData = await tokenResponse.json();
                    authToken = tokenData.token;
                    chrome.storage.local.set({ ldmAuthToken: authToken });
                }
            } catch (e) {
                console.log("Could not fetch auth token:", e);
            }

            isConnected = true;
            statusEl.innerHTML = `<span style="color: #4ade80;">●</span> Connected (v${data.version || '?'})`;
            btn.disabled = false;
            offlineNotice.classList.remove('show');

            // Update badge
            chrome.runtime.sendMessage({ action: "updateBadge", connected: true });
            return;
        }
    } catch (e) {
        // Connection failed
    }

    isConnected = false;
    statusEl.innerHTML = '<span style="color: #f87171;">●</span> LDM Offline';
    btn.disabled = true;
    offlineNotice.classList.add('show');

    chrome.runtime.sendMessage({ action: "updateBadge", connected: false });
}

// ============================================
// SETTINGS
// ============================================

function loadSettings() {
    chrome.storage.sync.get(['interceptAll'], function(result) {
        const isEnabled = result.interceptAll !== undefined ? result.interceptAll : true;
        document.getElementById('interceptToggle').checked = isEnabled;
    });
}

// ============================================
// STATS
// ============================================

function loadStats() {
    chrome.storage.local.get(['stats'], function(result) {
        const stats = result.stats || { totalDownloads: 0, totalBytes: 0, todayDownloads: 0, lastDate: null };

        // Reset today count if it's a new day
        const today = new Date().toDateString();
        if (stats.lastDate !== today) {
            stats.todayDownloads = 0;
            stats.lastDate = today;
            chrome.storage.local.set({ stats: stats });
        }

        document.getElementById('statDownloads').textContent = stats.totalDownloads || 0;
        document.getElementById('statToday').textContent = stats.todayDownloads || 0;

        // Format data size
        const bytes = stats.totalBytes || 0;
        let dataStr;
        if (bytes >= 1024 * 1024 * 1024) {
            dataStr = (bytes / (1024 * 1024 * 1024)).toFixed(1) + ' GB';
        } else if (bytes >= 1024 * 1024) {
            dataStr = (bytes / (1024 * 1024)).toFixed(1) + ' MB';
        } else if (bytes >= 1024) {
            dataStr = (bytes / 1024).toFixed(1) + ' KB';
        } else {
            dataStr = bytes + ' B';
        }
        document.getElementById('statData').textContent = dataStr;
    });
}

function updateStats(url, success) {
    chrome.storage.local.get(['stats'], function(result) {
        const stats = result.stats || { totalDownloads: 0, totalBytes: 0, todayDownloads: 0, lastDate: null };
        const today = new Date().toDateString();

        if (success) {
            stats.totalDownloads++;
            stats.todayDownloads++;
            stats.lastDate = today;
        }

        chrome.storage.local.set({ stats: stats });
        loadStats();
    });
}

// ============================================
// ACTIVE DOWNLOADS STATUS
// ============================================

function startStatusPolling() {
    // Fetch immediately
    fetchDownloadStatus();

    // Then poll every 1 second when connected
    statusInterval = setInterval(function() {
        if (isConnected) {
            fetchDownloadStatus();
        }
    }, 1000);
}

async function fetchDownloadStatus() {
    if (!isConnected) {
        renderActiveDownloads({ activeDownloads: 0, totalSpeed: 0, downloads: [] });
        return;
    }

    try {
        const response = await fetch(LDM_URL + "/status", { method: "GET", mode: "cors" });
        if (response.ok) {
            const data = await response.json();
            renderActiveDownloads(data);
        }
    } catch (e) {
        // Connection lost
        renderActiveDownloads({ activeDownloads: 0, totalSpeed: 0, downloads: [] });
    }
}

function renderActiveDownloads(data) {
    const container = document.getElementById('activeList');
    const speedBadge = document.getElementById('totalSpeedBadge');

    // Update total speed badge
    speedBadge.textContent = formatSpeed(data.totalSpeed || 0);

    // Update Active tab badge - sanitize the count
    const activeTab = document.querySelector('[data-tab="active"]');
    const count = parseInt(data.activeDownloads) || 0;
    if (count > 0) {
        activeTab.innerHTML = `<span class="tab-icon">&#9889;</span>Active (${count})`;
    } else {
        activeTab.innerHTML = `<span class="tab-icon">&#9889;</span>Active`;
    }

    if (!data.downloads || data.downloads.length === 0) {
        container.innerHTML = `
            <div class="empty-state">
                <div class="empty-state-icon">&#128260;</div>
                <div class="empty-state-text">No active downloads</div>
            </div>
        `;
        return;
    }

    // Clear and rebuild safely
    container.innerHTML = '';

    data.downloads.forEach(dl => {
        const icon = getFileIcon(dl.filename || 'file');
        const progress = parseFloat(dl.progress) || 0;
        const speed = formatSpeed(dl.speed || 0);
        const downloaded = formatSize(dl.downloaded || 0);
        const total = formatSize(dl.size || 0);

        const item = document.createElement('div');
        item.className = 'active-item';

        const header = document.createElement('div');
        header.className = 'active-header';

        const iconEl = document.createElement('div');
        iconEl.className = 'active-icon';
        iconEl.innerHTML = icon;

        const info = document.createElement('div');
        info.className = 'active-info';

        const nameEl = document.createElement('div');
        nameEl.className = 'active-name';
        nameEl.textContent = dl.filename || 'Downloading...';
        nameEl.title = dl.filename || '';

        const stats = document.createElement('div');
        stats.className = 'active-stats';

        const speedSpan = document.createElement('span');
        speedSpan.className = 'active-speed';
        speedSpan.textContent = speed;

        const sizeSpan = document.createElement('span');
        sizeSpan.textContent = `${downloaded} / ${total}`;

        const progressSpan = document.createElement('span');
        progressSpan.textContent = `${progress.toFixed(1)}%`;

        stats.appendChild(speedSpan);
        stats.appendChild(sizeSpan);
        stats.appendChild(progressSpan);

        info.appendChild(nameEl);
        info.appendChild(stats);

        header.appendChild(iconEl);
        header.appendChild(info);

        const progressBar = document.createElement('div');
        progressBar.className = 'progress-bar';

        const progressFill = document.createElement('div');
        progressFill.className = 'progress-fill';
        progressFill.style.width = `${Math.min(100, Math.max(0, progress))}%`;

        progressBar.appendChild(progressFill);

        item.appendChild(header);
        item.appendChild(progressBar);
        container.appendChild(item);
    });
}

function formatSpeed(bytesPerSecond) {
    if (bytesPerSecond >= 1024 * 1024) {
        return (bytesPerSecond / (1024 * 1024)).toFixed(1) + ' MB/s';
    } else if (bytesPerSecond >= 1024) {
        return (bytesPerSecond / 1024).toFixed(1) + ' KB/s';
    } else {
        return Math.round(bytesPerSecond) + ' B/s';
    }
}

function formatSize(bytes) {
    if (bytes >= 1024 * 1024 * 1024) {
        return (bytes / (1024 * 1024 * 1024)).toFixed(1) + ' GB';
    } else if (bytes >= 1024 * 1024) {
        return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
    } else if (bytes >= 1024) {
        return (bytes / 1024).toFixed(1) + ' KB';
    } else {
        return bytes + ' B';
    }
}

// ============================================
// RECENT DOWNLOADS
// ============================================

function loadRecentDownloads() {
    chrome.storage.local.get(['recentDownloads'], function(result) {
        const recent = result.recentDownloads || [];
        const container = document.getElementById('recentList');

        if (recent.length === 0) {
            container.innerHTML = `
                <div class="empty-state">
                    <div class="empty-state-icon">&#128230;</div>
                    <div class="empty-state-text">No recent downloads</div>
                </div>
            `;
            return;
        }

        // Clear and rebuild safely
        container.innerHTML = '';

        recent.slice(0, 10).forEach(item => {
            const icon = getFileIcon(item.url);
            const name = getFilenameFromUrl(item.url);
            const time = formatTime(item.timestamp);

            const itemEl = document.createElement('div');
            itemEl.className = 'recent-item';

            const iconEl = document.createElement('div');
            iconEl.className = 'recent-icon';
            iconEl.innerHTML = icon;

            const infoEl = document.createElement('div');
            infoEl.className = 'recent-info';

            const nameEl = document.createElement('div');
            nameEl.className = 'recent-name';
            nameEl.textContent = name;
            nameEl.title = item.url;

            const timeEl = document.createElement('div');
            timeEl.className = 'recent-time';
            timeEl.textContent = time;

            infoEl.appendChild(nameEl);
            infoEl.appendChild(timeEl);

            const statusEl = document.createElement('div');
            statusEl.className = 'recent-status';
            statusEl.innerHTML = item.success ? '&#10004;' : '&#10008;';
            statusEl.style.color = item.success ? '#10b981' : '#ef4444';

            itemEl.appendChild(iconEl);
            itemEl.appendChild(infoEl);
            itemEl.appendChild(statusEl);
            container.appendChild(itemEl);
        });
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
        // Keep only last 50
        chrome.storage.local.set({ recentDownloads: recent.slice(0, 50) });
        loadRecentDownloads();
    });
}

function clearHistory() {
    chrome.storage.local.set({ recentDownloads: [] });
    loadRecentDownloads();
    showStatus("History cleared", "success");
    playSound('success');
}

// ============================================
// DOWNLOAD FUNCTIONS
// ============================================

async function sendToLDM(url, referer = null) {
    const btn = document.getElementById('downloadBtn');
    const originalText = btn.innerHTML;

    btn.innerHTML = '<span>&#8987;</span> Sending...';
    btn.disabled = true;

    // Build request body with optional referer
    const requestBody = { url: url };
    if (referer) {
        requestBody.referer = referer;
    }

    try {
        if (!authToken) {
            const tokenResponse = await fetch(LDM_URL + "/token", { method: "GET", mode: "cors" });
            if (tokenResponse.ok) {
                const tokenData = await tokenResponse.json();
                authToken = tokenData.token;
                chrome.storage.local.set({ ldmAuthToken: authToken });
            }
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
            showStatus("Download started!", "success");
            document.getElementById('manualUrl').value = "";
            addToRecent(url, true);
            updateStats(url, true);
            playSound('success');

            // Show notification
            chrome.runtime.sendMessage({
                action: "showNotification",
                title: "Download Started",
                message: getFilenameFromUrl(url)
            });
        } else if (response.status === 401) {
            authToken = null;
            // Retry once after refreshing token
            try {
                const tokenResponse = await fetch(LDM_URL + "/token", { method: "GET", mode: "cors" });
                if (tokenResponse.ok) {
                    const tokenData = await tokenResponse.json();
                    authToken = tokenData.token;
                    chrome.storage.local.set({ ldmAuthToken: authToken });

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
                        showStatus("Download started!", "success");
                        document.getElementById('manualUrl').value = "";
                        addToRecent(url, true);
                        updateStats(url, true);
                        playSound('success');

                        chrome.runtime.sendMessage({
                            action: "showNotification",
                            title: "Download Started",
                            message: getFilenameFromUrl(url)
                        });
                        btn.innerHTML = originalText;
                        btn.disabled = !isConnected;
                        return;
                    }
                }
            } catch (e) {
                // Fall through to show auth error
            }

            showStatus("Auth error - try again", "error");
            playSound('error');
        } else {
            showStatus("Error: " + response.status, "error");
            addToRecent(url, false);
            playSound('error');
        }
    } catch (e) {
        showStatus("LDM not running", "error");
        addToRecent(url, false);
        checkConnection();
        playSound('error');
    }

    btn.innerHTML = originalText;
    btn.disabled = !isConnected;
}

async function downloadBulk() {
    const textarea = document.getElementById('bulkUrls');
    const urls = textarea.value.split('\n')
        .map(url => url.trim())
        .filter(url => url.length > 0 && (url.startsWith('http://') || url.startsWith('https://')));

    if (urls.length === 0) {
        showStatus("No valid URLs found", "error");
        return;
    }

    let successCount = 0;
    let failCount = 0;

    for (const url of urls) {
        try {
            const response = await fetch(LDM_URL + "/download", {
                method: "POST",
                mode: "cors",
                headers: {
                    "Content-Type": "application/json",
                    "X-Auth-Token": authToken || ""
                },
                body: JSON.stringify({ url: url })
            });

            if (response.ok) {
                successCount++;
                addToRecent(url, true);
            } else {
                failCount++;
                addToRecent(url, false);
            }
        } catch (e) {
            failCount++;
        }
    }

    updateStats(null, true);
    textarea.value = "";
    updateUrlCount();

    if (failCount === 0) {
        showStatus(`${successCount} downloads started!`, "success");
        playSound('success');
    } else {
        showStatus(`${successCount} started, ${failCount} failed`, "error");
        playSound('error');
    }
}

// ============================================
// VIDEO DETECTION
// ============================================

function scanForVideos() {
    chrome.tabs.query({ active: true, currentWindow: true }, function(tabs) {
        if (!tabs[0]) return;

        chrome.tabs.sendMessage(tabs[0].id, { action: "scanVideos" }, function(response) {
            const container = document.getElementById('videoList');

            if (chrome.runtime.lastError || !response || !response.videos || response.videos.length === 0) {
                container.innerHTML = `
                    <div class="empty-state">
                        <div class="empty-state-icon">&#127909;</div>
                        <div class="empty-state-text">No videos detected on this page</div>
                    </div>
                `;
                return;
            }

            // Clear container
            container.innerHTML = '';

            response.videos.forEach((video, index) => {
                const item = document.createElement('div');
                item.className = 'video-item';

                const thumb = document.createElement('div');
                thumb.className = 'video-thumb';
                // Use different icon for yt-dlp page downloads
                if (video.type === 'page') {
                    thumb.innerHTML = '&#127760;';  // Globe icon for page downloads
                    thumb.title = 'Download via yt-dlp';
                } else {
                    thumb.innerHTML = '&#127909;';  // Video icon
                }

                const info = document.createElement('div');
                info.className = 'video-info';

                const title = document.createElement('div');
                title.className = 'video-title';
                title.textContent = video.title || 'Video ' + (index + 1);

                const quality = document.createElement('div');
                quality.className = 'video-quality';
                if (video.type === 'page') {
                    quality.textContent = 'Page Video (yt-dlp)';
                    quality.style.color = '#10b981';  // Green to indicate yt-dlp support
                } else {
                    quality.textContent = video.quality || 'Unknown quality';
                }

                info.appendChild(title);
                info.appendChild(quality);

                const btn = document.createElement('button');
                btn.className = 'video-btn';
                btn.innerHTML = '&#9660;';
                btn.addEventListener('click', function() {
                    // Pass page URL as referer for protected video downloads
                    sendToLDM(video.url, tabs[0].url);
                });

                item.appendChild(thumb);
                item.appendChild(info);
                item.appendChild(btn);
                container.appendChild(item);
            });
        });
    });
}

// ============================================
// LINK GRABBING
// ============================================

function grabLinks(mediaOnly) {
    chrome.tabs.query({ active: true, currentWindow: true }, function(tabs) {
        if (!tabs[0]) return;

        chrome.tabs.sendMessage(tabs[0].id, {
            action: "grabLinks",
            mediaOnly: mediaOnly
        }, function(response) {
            const container = document.getElementById('grabbedLinks');

            if (chrome.runtime.lastError || !response || !response.links || response.links.length === 0) {
                container.innerHTML = `
                    <div class="empty-state">
                        <div class="empty-state-icon">&#128279;</div>
                        <div class="empty-state-text">No ${mediaOnly ? 'media ' : ''}links found</div>
                    </div>
                `;
                return;
            }

            // Show links in textarea for selection
            const textarea = document.getElementById('bulkUrls');
            textarea.value = response.links.join('\n');

            // Switch to URLs mode
            document.querySelector('[data-mode="urls"]').click();
            updateUrlCount();

            showStatus(`Found ${response.links.length} links`, "success");
        });
    });
}

// ============================================
// EVENT LISTENERS
// ============================================

function setupEventListeners() {
    // Theme toggle
    document.getElementById('themeToggle').addEventListener('click', toggleTheme);

    // Settings buttons
    document.getElementById('openOptions').addEventListener('click', openOptions);
    document.getElementById('openFullOptions').addEventListener('click', openOptions);

    // Intercept toggle
    document.getElementById('interceptToggle').addEventListener('change', function(e) {
        chrome.storage.sync.set({ interceptAll: e.target.checked }, function() {
            chrome.runtime.sendMessage({ action: "updateSettings" });
        });
        playSound('click');
    });

    document.getElementById('toggleContainer').addEventListener('click', function(e) {
        if (e.target.tagName !== 'INPUT' && e.target.className !== 'slider') {
            const checkbox = document.getElementById('interceptToggle');
            checkbox.checked = !checkbox.checked;
            checkbox.dispatchEvent(new Event('change'));
        }
    });

    // Download button
    document.getElementById('downloadBtn').addEventListener('click', function() {
        const url = document.getElementById('manualUrl').value.trim();
        if (url) {
            sendToLDM(url);
        } else {
            showStatus("Please enter a URL", "error");
        }
    });

    // Enter key in URL input
    document.getElementById('manualUrl').addEventListener('keypress', function(e) {
        if (e.key === 'Enter') {
            document.getElementById('downloadBtn').click();
        }
    });

    // Tabs
    document.querySelectorAll('.tab').forEach(tab => {
        tab.addEventListener('click', function() {
            document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
            this.classList.add('active');
            document.getElementById('tab-' + this.dataset.tab).classList.add('active');
            playSound('click');

            // Auto-scan for videos when Videos tab is opened
            if (this.dataset.tab === 'videos') {
                scanForVideos();
            }
        });
    });

    // Clear history
    document.getElementById('clearHistory').addEventListener('click', clearHistory);

    // Scan videos
    document.getElementById('scanVideos').addEventListener('click', scanForVideos);

    // Batch mode toggle
    document.querySelectorAll('.mode-btn').forEach(btn => {
        btn.addEventListener('click', function() {
            document.querySelectorAll('.mode-btn').forEach(b => b.classList.remove('active'));
            this.classList.add('active');

            const mode = this.dataset.mode;
            document.getElementById('batchUrls').style.display = mode === 'urls' ? 'block' : 'none';
            document.getElementById('batchLinks').style.display = mode === 'links' ? 'block' : 'none';
            playSound('click');
        });
    });

    // Bulk URLs textarea
    document.getElementById('bulkUrls').addEventListener('input', updateUrlCount);

    // Download bulk
    document.getElementById('downloadBulk').addEventListener('click', downloadBulk);

    // Grab links
    document.getElementById('grabAllLinks').addEventListener('click', () => grabLinks(false));
    document.getElementById('grabMediaLinks').addEventListener('click', () => grabLinks(true));

    // Drop zone
    const dropZone = document.getElementById('dropZone');

    dropZone.addEventListener('dragover', function(e) {
        e.preventDefault();
        this.classList.add('dragover');
    });

    dropZone.addEventListener('dragleave', function() {
        this.classList.remove('dragover');
    });

    dropZone.addEventListener('drop', function(e) {
        e.preventDefault();
        this.classList.remove('dragover');

        let url = '';
        if (e.dataTransfer.types.includes('text/uri-list')) {
            url = e.dataTransfer.getData('text/uri-list');
        } else if (e.dataTransfer.types.includes('text/plain')) {
            url = e.dataTransfer.getData('text/plain');
        }

        if (url && (url.startsWith('http://') || url.startsWith('https://'))) {
            sendToLDM(url);
        } else {
            showStatus("Invalid URL", "error");
        }
    });

    dropZone.addEventListener('click', function() {
        document.getElementById('manualUrl').focus();
    });
}

// ============================================
// UTILITY FUNCTIONS
// ============================================

function openOptions() {
    if (chrome.runtime.openOptionsPage) {
        chrome.runtime.openOptionsPage();
    } else {
        window.open(chrome.runtime.getURL('options/options.html'));
    }
}

function showStatus(message, type) {
    const status = document.getElementById('status');
    status.innerText = message;
    status.className = "status-message show " + type;

    setTimeout(function() {
        status.className = "status-message";
    }, 3000);
}

function updateUrlCount() {
    const textarea = document.getElementById('bulkUrls');
    const urls = textarea.value.split('\n').filter(url => url.trim().length > 0);
    document.getElementById('urlCount').textContent = urls.length;
}

function getFilenameFromUrl(url) {
    try {
        const pathname = new URL(url).pathname;
        const filename = pathname.split('/').pop() || 'download';
        return decodeURIComponent(filename).substring(0, 40);
    } catch (e) {
        return 'download';
    }
}

function getFileIcon(url) {
    const ext = url.split('.').pop().split('?')[0].toLowerCase();
    const icons = {
        // Video
        'mp4': '&#127909;', 'mkv': '&#127909;', 'avi': '&#127909;', 'mov': '&#127909;', 'webm': '&#127909;',
        // Audio
        'mp3': '&#127925;', 'wav': '&#127925;', 'flac': '&#127925;', 'aac': '&#127925;', 'ogg': '&#127925;',
        // Images
        'jpg': '&#128247;', 'jpeg': '&#128247;', 'png': '&#128247;', 'gif': '&#128247;', 'webp': '&#128247;',
        // Documents
        'pdf': '&#128196;', 'doc': '&#128196;', 'docx': '&#128196;', 'txt': '&#128196;',
        // Archives
        'zip': '&#128451;', 'rar': '&#128451;', '7z': '&#128451;', 'tar': '&#128451;',
        // Executables
        'exe': '&#128190;', 'msi': '&#128190;', 'dmg': '&#128190;'
    };
    return icons[ext] || '&#128196;';
}

function formatTime(timestamp) {
    const diff = Date.now() - timestamp;
    const minutes = Math.floor(diff / 60000);
    const hours = Math.floor(diff / 3600000);
    const days = Math.floor(diff / 86400000);

    if (minutes < 1) return 'Just now';
    if (minutes < 60) return minutes + 'm ago';
    if (hours < 24) return hours + 'h ago';
    return days + 'd ago';
}

function playSound(type) {
    // soundEnabled is stored in chrome.storage.sync
    chrome.storage.sync.get(['soundEnabled'], function(result) {
        // Default to true if not set
        if (result.soundEnabled === false) return;

        // Use Web Audio API for simple beep sounds
        try {
            const audioContext = new (window.AudioContext || window.webkitAudioContext)();
            const oscillator = audioContext.createOscillator();
            const gainNode = audioContext.createGain();

            oscillator.connect(gainNode);
            gainNode.connect(audioContext.destination);

            oscillator.type = 'sine';
            gainNode.gain.value = 0.1;

            if (type === 'success') {
                oscillator.frequency.value = 800;
            } else if (type === 'error') {
                oscillator.frequency.value = 300;
            } else {
                oscillator.frequency.value = 600;
            }

            oscillator.start();
            setTimeout(() => {
                oscillator.stop();
                audioContext.close();
            }, 100);
        } catch (e) {
            // Audio not supported
        }
    });
}

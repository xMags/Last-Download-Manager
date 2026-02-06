// LDM Options Page v3.0

const LDM_URL = "http://127.0.0.1:45678";

// ============================================
// INITIALIZATION
// ============================================

document.addEventListener('DOMContentLoaded', function() {
    loadTheme();
    loadSettings();
    loadStats();
    checkConnection();
    setupEventListeners();
});

// ============================================
// THEME
// ============================================

function loadTheme() {
    chrome.storage.local.get(['darkMode', 'colorTheme'], function(result) {
        if (result.darkMode) {
            document.documentElement.classList.add('dark');
            updateThemeButton(true);
        }

        // Apply color theme
        if (result.colorTheme) {
            setColorTheme(result.colorTheme);
            document.querySelectorAll('.theme-option').forEach(opt => {
                opt.classList.toggle('active', opt.dataset.theme === result.colorTheme);
            });
        } else {
            document.querySelector('[data-theme="indigo"]').classList.add('active');
        }
    });
}

function toggleDarkMode() {
    const isDark = document.documentElement.classList.toggle('dark');
    chrome.storage.local.set({ darkMode: isDark });
    updateThemeButton(isDark);
}

function updateThemeButton(isDark) {
    document.getElementById('themeIcon').innerHTML = isDark ? '&#9728;' : '&#127769;';
    document.getElementById('themeToggle').innerHTML =
        `<span id="themeIcon">${isDark ? '&#9728;' : '&#127769;'}</span> ${isDark ? 'Light' : 'Dark'} Mode`;
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
// SETTINGS
// ============================================

function loadSettings() {
    chrome.storage.sync.get({
        interceptAll: true,
        ignoredExtensions: ['jpg', 'jpeg', 'png', 'gif', 'bmp', 'webp'],
        minFileSize: 0,
        domainWhitelist: [],
        domainBlacklist: [],
        notificationsEnabled: true,
        soundEnabled: true
    }, function(items) {
        document.getElementById('interceptAll').checked = items.interceptAll;
        document.getElementById('notificationsEnabled').checked = items.notificationsEnabled;
        document.getElementById('soundEnabled').checked = items.soundEnabled;
        document.getElementById('ignoredExtensions').value = items.ignoredExtensions.join('\n');
        document.getElementById('minFileSize').value = items.minFileSize || 0;
        document.getElementById('domainWhitelist').value = (items.domainWhitelist || []).join('\n');
        document.getElementById('domainBlacklist').value = (items.domainBlacklist || []).join('\n');
    });
}

function saveSettings() {
    const settings = {
        interceptAll: document.getElementById('interceptAll').checked,
        notificationsEnabled: document.getElementById('notificationsEnabled').checked,
        soundEnabled: document.getElementById('soundEnabled').checked,
        ignoredExtensions: parseTextarea('ignoredExtensions'),
        minFileSize: parseInt(document.getElementById('minFileSize').value) || 0,
        domainWhitelist: parseTextarea('domainWhitelist'),
        domainBlacklist: parseTextarea('domainBlacklist')
    };

    chrome.storage.sync.set(settings, function() {
        chrome.runtime.sendMessage({ action: "updateSettings" });
        showToast("Settings saved!");
    });
}

function parseTextarea(id) {
    return document.getElementById(id).value
        .split('\n')
        .map(line => line.trim().toLowerCase().replace(/^\./, ''))
        .filter(line => line.length > 0);
}

// ============================================
// STATS
// ============================================

function loadStats() {
    chrome.storage.local.get(['stats'], function(result) {
        const stats = result.stats || { totalDownloads: 0, totalBytes: 0, todayDownloads: 0 };

        document.getElementById('statTotal').textContent = stats.totalDownloads || 0;
        document.getElementById('statToday').textContent = stats.todayDownloads || 0;

        const bytes = stats.totalBytes || 0;
        let dataStr;
        if (bytes >= 1024 * 1024 * 1024) {
            dataStr = (bytes / (1024 * 1024 * 1024)).toFixed(1) + ' GB';
        } else if (bytes >= 1024 * 1024) {
            dataStr = (bytes / (1024 * 1024)).toFixed(1) + ' MB';
        } else {
            dataStr = '0 MB';
        }
        document.getElementById('statData').textContent = dataStr;
    });
}

async function checkConnection() {
    const statusEl = document.getElementById('statStatus');
    try {
        const response = await fetch(LDM_URL + "/ping", { method: "GET", mode: "cors" });
        if (response.ok) {
            const data = await response.json();
            statusEl.textContent = 'Online';
            statusEl.style.color = '#10b981';
            return;
        }
    } catch (e) {}
    statusEl.textContent = 'Offline';
    statusEl.style.color = '#ef4444';
}

// ============================================
// DATA MANAGEMENT
// ============================================

function exportSettings() {
    chrome.storage.sync.get(null, function(syncData) {
        chrome.storage.local.get(null, function(localData) {
            const exportData = {
                version: '3.0',
                timestamp: new Date().toISOString(),
                sync: syncData,
                local: {
                    darkMode: localData.darkMode,
                    colorTheme: localData.colorTheme,
                    soundEnabled: localData.soundEnabled
                }
            };

            const blob = new Blob([JSON.stringify(exportData, null, 2)], { type: 'application/json' });
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = `ldm-settings-${new Date().toISOString().split('T')[0]}.json`;
            a.click();
            URL.revokeObjectURL(url);

            showToast("Settings exported!");
        });
    });
}

function importSettings() {
    document.getElementById('importFile').click();
}

function handleImport(event) {
    const file = event.target.files[0];
    if (!file) return;

    const reader = new FileReader();
    reader.onload = function(e) {
        try {
            const data = JSON.parse(e.target.result);

            if (data.sync) {
                chrome.storage.sync.set(data.sync, function() {
                    loadSettings();
                });
            }

            if (data.local) {
                chrome.storage.local.set(data.local, function() {
                    loadTheme();
                });
            }

            chrome.runtime.sendMessage({ action: "updateSettings" });
            showToast("Settings imported!");
        } catch (err) {
            showToast("Invalid settings file");
        }
    };
    reader.readAsText(file);
    event.target.value = '';
}

function resetStats() {
    if (!confirm("Reset all statistics? This cannot be undone.")) return;

    chrome.storage.local.set({
        stats: { totalDownloads: 0, totalBytes: 0, todayDownloads: 0, lastDate: null }
    }, function() {
        loadStats();
        showToast("Statistics reset!");
    });
}

function clearHistory() {
    if (!confirm("Clear download history? This cannot be undone.")) return;

    chrome.storage.local.set({ recentDownloads: [] }, function() {
        showToast("History cleared!");
    });
}

function resetAll() {
    if (!confirm("Reset ALL extension data including settings and history? This cannot be undone.")) return;
    if (!confirm("Are you really sure? This will reset everything to defaults.")) return;

    chrome.storage.sync.clear();
    chrome.storage.local.clear();

    showToast("Everything reset!");
    setTimeout(() => location.reload(), 1500);
}

// ============================================
// EVENT LISTENERS
// ============================================

function setupEventListeners() {
    // Theme toggle
    document.getElementById('themeToggle').addEventListener('click', toggleDarkMode);

    // Tabs
    document.querySelectorAll('.tab-btn').forEach(btn => {
        btn.addEventListener('click', function() {
            document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
            document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
            this.classList.add('active');
            document.getElementById('tab-' + this.dataset.tab).classList.add('active');
        });
    });

    // Save button
    document.getElementById('saveSettings').addEventListener('click', saveSettings);

    // Data management
    document.getElementById('exportSettings').addEventListener('click', exportSettings);
    document.getElementById('importSettings').addEventListener('click', importSettings);
    document.getElementById('importFile').addEventListener('change', handleImport);
    document.getElementById('resetStats').addEventListener('click', resetStats);
    document.getElementById('clearHistory').addEventListener('click', clearHistory);
    document.getElementById('resetAll').addEventListener('click', resetAll);

    // Theme options
    document.querySelectorAll('.theme-option').forEach(opt => {
        opt.addEventListener('click', function() {
            document.querySelectorAll('.theme-option').forEach(o => o.classList.remove('active'));
            this.classList.add('active');
            const theme = this.dataset.theme;
            setColorTheme(theme);
            chrome.storage.local.set({ colorTheme: theme });
        });
    });

    // Setting rows - click to toggle
    document.querySelectorAll('.setting-row').forEach(row => {
        row.addEventListener('click', function(e) {
            if (e.target.type !== 'checkbox' && e.target.className !== 'slider') {
                const checkbox = this.querySelector('input[type="checkbox"]');
                if (checkbox) {
                    checkbox.checked = !checkbox.checked;
                }
            }
        });
    });

    // Keyboard shortcut: Ctrl+S to save
    document.addEventListener('keydown', function(e) {
        if ((e.ctrlKey || e.metaKey) && e.key === 's') {
            e.preventDefault();
            saveSettings();
        }
    });
}

// ============================================
// TOAST
// ============================================

function showToast(message) {
    const toast = document.getElementById('toast');
    document.getElementById('toastMessage').textContent = message;
    toast.classList.add('show');

    setTimeout(function() {
        toast.classList.remove('show');
    }, 3000);
}

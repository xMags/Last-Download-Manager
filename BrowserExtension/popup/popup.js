const LDM_URL = "http://127.0.0.1:45678";

document.addEventListener('DOMContentLoaded', function() {
    // Check LDM connection status
    checkConnection();
    
    // 1. Load initial state
    chrome.storage.sync.get(['interceptAll'], function(result) {
        var isEnabled = (result.interceptAll !== undefined) ? result.interceptAll : true;
        document.getElementById('interceptToggle').checked = isEnabled;
    });

    // 2. Handle Toggle
    document.getElementById('interceptToggle').addEventListener('change', function(e) {
        var isEnabled = e.target.checked;
        chrome.storage.sync.set({interceptAll: isEnabled}, function() {
            chrome.runtime.sendMessage({action: "updateSettings"});
        });
    });
    
    // Also toggle when clicking the container text
    document.getElementById('toggleContainer').addEventListener('click', function(e) {
        if (e.target.tagName !== 'INPUT' && e.target.className !== 'slider') {
            var checkbox = document.getElementById('interceptToggle');
            checkbox.checked = !checkbox.checked;
            checkbox.dispatchEvent(new Event('change'));
        }
    });

    // 3. Handle Manual Download
    document.getElementById('downloadBtn').addEventListener('click', function() {
        var url = document.getElementById('manualUrl').value.trim();
        if (url) {
            sendToLDM(url);
        }
    });
    
    // Allow pressing Enter in text box
    document.getElementById('manualUrl').addEventListener('keypress', function(e) {
        if (e.key === 'Enter') {
            document.getElementById('downloadBtn').click();
        }
    });

    // 4. Open Options Page
    document.getElementById('openOptions').addEventListener('click', function() {
        if (chrome.runtime.openOptionsPage) {
            chrome.runtime.openOptionsPage();
        } else {
            window.open(chrome.runtime.getURL('options/options.html'));
        }
    });
});

async function checkConnection() {
    const statusEl = document.getElementById('connectionStatus');
    const textEl = document.getElementById('connectionText');
    const btn = document.getElementById('downloadBtn');
    const offlineNotice = document.getElementById('offlineNotice');
    
    try {
        const response = await fetch(LDM_URL + "/ping", { method: "GET", mode: "cors" });
        if (response.ok) {
            statusEl.className = "connection-status connected";
            textEl.innerText = "Connected";
            btn.disabled = false;
            offlineNotice.classList.remove('show');
            return;
        }
    } catch (e) {
        // Connection failed
    }
    
    statusEl.className = "connection-status disconnected";
    textEl.innerText = "Offline";
    btn.disabled = true;
    offlineNotice.classList.add('show');
}

async function sendToLDM(url) {
    var status = document.getElementById('status');
    
    try {
        const response = await fetch(LDM_URL + "/download", {
            method: "POST",
            mode: "cors",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ url: url })
        });
        
        if (response.ok) {
            status.innerText = "Sent to LDM!";
            status.style.color = "green";
            document.getElementById('manualUrl').value = "";
        } else {
            status.innerText = "Error: " + response.status;
            status.style.color = "red";
        }
    } catch (e) {
        status.innerText = "LDM not running";
        status.style.color = "red";
        checkConnection(); // Update status indicator
    }
    
    status.style.opacity = 1;
    setTimeout(function() {
        status.style.opacity = 0;
    }, 2000);
}

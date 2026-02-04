// Save options to chrome.storage
function saveOptions() {
    var interceptAll = document.getElementById('interceptAll').checked;
    var ignoredExtensions = document.getElementById('ignoredExtensions').value;
    
    // Clean up extensions list
    var extensionsList = ignoredExtensions.split('\n')
        .map(ext => ext.trim().toLowerCase().replace(/^\./, '')) // trim, lowercase, remove leading dot
        .filter(ext => ext.length > 0);

    chrome.storage.sync.set({
        interceptAll: interceptAll,
        ignoredExtensions: extensionsList
    }, function() {
        var status = document.getElementById('status');
        status.style.display = 'block';
        setTimeout(function() {
            status.style.display = 'none';
        }, 2000);
        
        // Notify background script to update settings immediately
        chrome.runtime.sendMessage({action: "updateSettings"});
    });
}

// Restore select box and checkbox state using the preferences stored in chrome.storage.
function restoreOptions() {
    chrome.storage.sync.get({
        interceptAll: true, // Default true
        ignoredExtensions: ['jpg', 'jpeg', 'png', 'gif', 'bmp', 'webp'] // Default image formats
    }, function(items) {
        document.getElementById('interceptAll').checked = items.interceptAll;
        document.getElementById('ignoredExtensions').value = items.ignoredExtensions.join('\n');
    });
}

document.addEventListener('DOMContentLoaded', restoreOptions);
document.getElementById('save').addEventListener('click', saveOptions);

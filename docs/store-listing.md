# Browser store listings

The texts for the Chrome Web Store, Microsoft Edge Add-ons and Firefox Add-ons, kept here so every
store says the same thing. The packages are `LastDownloadManager-chromium.zip` (Chrome and Edge)
and `LastDownloadManager-firefox.zip` from a release build (`docs/releasing.md`).

## Name

Last Download Manager integration

## Short description (up to 132 characters)

Hands your browser's downloads, links and video streams to Last Download Manager, a free download
manager for Windows.

## Description

Last Download Manager (LDM) is a free, open source download manager for Windows. This extension
connects your browser to it:

- Downloads of the file types you choose go to LDM, which downloads them over several connections
  at once, resumes them after a broken connection or a restart, and keeps them in one list.
- "Download with LDM", "Download all links with LDM" and "Download selected links with LDM" in the
  right-click menu.
- A download button over videos and audio the page plays, for files and for HLS and DASH streams.
- Keys you can hold to keep a link in the browser or give it to LDM.

The extension needs the LDM program, from https://github.com/xMags/Last-Download-Manager. It talks
only to LDM on your own computer. Nothing is sent to the project or to anyone else.

Privacy policy: https://github.com/xMags/Last-Download-Manager/blob/main/docs/privacy-policy.md

## Category

Productivity (Chrome, Edge); Download Management (Firefox)

## Why each permission is needed

| Permission | Why |
|---|---|
| `nativeMessaging` | The only way the extension talks to LDM: through the helper program LDM installs, on the same computer. |
| `downloads` | To see a download the browser starts and hand it to LDM instead, or leave it to the browser when LDM refuses it. |
| `downloads.ui` (Chrome, Edge) | To keep the browser's download bubble from flashing for a download LDM takes over. |
| `cookies` | A download often needs the site's sign-in. The extension reads the cookies the browser would send to that download's address and gives them to LDM, on this computer only. |
| `webRequest` | To notice the video and audio streams a page loads, for the download button over the player. Nothing is changed or blocked. |
| `scripting` | To find the links of a page for "Download all links", and where the video sits on the page for the download button. |
| `tabs` | To know which tab a download or a stream belongs to, and to open the page a download came from when LDM asks to refresh its address. |
| `contextMenus` | The "Download with LDM" items in the right-click menu. |
| `storage` | To remember LDM's capture settings while LDM is not running. |
| `alarms` | To reconnect to LDM when it starts after the browser. |
| Host access to all sites | Downloads, links and videos can be on any site; the extension only reads what it hands to LDM, when it hands it over. |

## Screenshots

Take them from the running extension and LDM on a release build: the right-click menu with the LDM
items, the download button over a video, LDM's list with a download it took over, and the Download
All Links dialog. Size 1280 by 800.

## After the first upload

The Chrome Web Store and Edge Add-ons give the extension an id of their own. Add each to
`kChromiumExtensionIds` in `src/browser/HostManifest.h` and release LDM again, so the helper lets
the published extension in. Firefox keeps the id in `manifest.firefox.json`.

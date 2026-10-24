# Privacy policy

Last Download Manager (LDM) and its browser extension run on your own computer. Neither collects
analytics, and neither sends anything about you to the project or to anyone else.

## What the browser extension does with your data

The extension talks only to the LDM program on the same computer, through the browser's native
messaging (a local connection that never leaves the machine). It sends LDM:

- the address, name, size and type of a download it hands over, the page it started from, the
  browser's User-Agent, and the cookies the browser would send to that address, so the download
  works as it would in the browser;
- for a download that answers a form, the form's content, so LDM can send it again;
- for "Download all links" and "Download selected links", the links of the page and their texts;
- for the media panel, the addresses of the video and audio streams a tab loads, the page's
  address and title, and where the video is on the screen, so LDM can place its button over it;
- for a Site Grabber project that asks for it, the cookies the browser would send to the project's
  start page.

LDM keeps download addresses, names and cookies in its own database in your user profile. Cookies,
passwords and form contents are encrypted there with Windows' data protection for your user
account. Nothing of it is sent anywhere except to the sites you download from, as part of those
downloads.

The extension stores LDM's capture settings in the browser's extension storage, so it can decide
which downloads to hand over while LDM is not running.

## What LDM sends over the internet

- Requests to the sites you download from, the pages a Site Grabber project explores, and the
  streams you choose, carrying the cookies and logins that belong to them.
- Only if you turn it on: every two weeks, one request to GitHub for the latest release of LDM
  (`https://api.github.com/repos/xMags/Last-Download-Manager/releases/latest`), with LDM's
  User-Agent and no identifier. LDM shows a notice when a newer version exists; it never downloads
  or installs anything by itself.

## Contact

Questions about this policy: open an issue at
<https://github.com/xMags/Last-Download-Manager/issues>.

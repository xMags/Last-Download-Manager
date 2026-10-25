// The items the extension adds to the browser's right-click menu (checklist B-04). Which appear
// is set per browser in LDM's Options >
// General > Context menu and arrives in the config (protocol.js menuSettings).

export const MenuId = Object.freeze({
  Link: 'ldm-link',            // a link, picture, video or sound
  Page: 'ldm-page',            // the page itself, e.g. a file the browser shows
  Selected: 'ldm-selected',
  All: 'ldm-all',
  VideoPage: 'ldm-video-page',  // the video on the page, found by LDM's site resolver
  VideoLink: 'ldm-video-link',  // the video on the page a link leads to
});

const WEB_PAGES = ['http://*/*', 'https://*/*'];

/**
 * The menu items to create, as contextMenus.create options. Nothing is added to links in Chrome's
 * downloads page: current Chrome shows no link for a running download there, and a finished one
 * links to the file on disk. The video items appear while LDM takes site requests (site videos
 * on, in an LDM that sends its list of sites).
 * @param {{downloadLink: boolean, downloadAll: boolean, videos?: boolean}} menu
 */
export function menuItems(menu) {
  const items = [];
  if (menu.downloadLink) {
    items.push(
      { id: MenuId.Link, title: 'Download with LDM', contexts: ['link', 'image', 'video', 'audio'],
        documentUrlPatterns: [...WEB_PAGES, 'file:///*'] },
      { id: MenuId.Page, title: 'Download with LDM', contexts: ['page', 'frame'], documentUrlPatterns: WEB_PAGES },
      { id: MenuId.Selected, title: 'Download selected links with LDM', contexts: ['selection'],
        documentUrlPatterns: [...WEB_PAGES, 'file:///*'] },
    );
  }
  if (menu.downloadAll) {
    items.push({ id: MenuId.All, title: 'Download all links with LDM', contexts: ['page', 'frame'],
      documentUrlPatterns: [...WEB_PAGES, 'file:///*'] });
  }
  if (menu.videos) {
    items.push(
      { id: MenuId.VideoPage, title: 'Download video with LDM', contexts: ['page', 'frame', 'video'],
        documentUrlPatterns: WEB_PAGES },
      { id: MenuId.VideoLink, title: 'Download video from this link with LDM', contexts: ['link'],
        documentUrlPatterns: WEB_PAGES, targetUrlPatterns: WEB_PAGES },
    );
  }
  return items;
}

/** The page a "Download video" click is about: the link's target, or the page (frame) itself. */
export function clickedVideoPage(info) {
  if (info.menuItemId === MenuId.VideoLink) {
    return info.linkUrl;
  }
  return info.frameUrl || info.pageUrl;
}

/** The address a "Download with LDM" click is about, from contextMenus.onClicked's info. */
export function clickedAddress(info) {
  if (info.menuItemId === MenuId.Page) {
    return info.frameUrl || info.pageUrl;
  }
  // A link around a picture: the link.
  return info.linkUrl || info.srcUrl;
}

/** Replaces the menu with `items`. Items a browser refuses (an unknown pattern) are left out. */
export async function applyMenu(api, items) {
  await api.contextMenus.removeAll();
  for (const item of items) {
    try {
      api.contextMenus.create(item, () => {
        const error = api.runtime.lastError;
        if (error) {
          console.warn('LDM: menu item refused', item.id, error.message);
        }
      });
    } catch (error) {
      console.warn('LDM: menu item refused', item.id, error);
    }
  }
}

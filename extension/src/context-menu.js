// The items the extension adds to the browser's right-click menu (checklist B-04): the same as
// the reference's extension, with LDM in place of the reference. Which appear is set per browser in LDM's Options >
// General > Context menu and arrives in the config (protocol.js menuSettings).

export const MenuId = Object.freeze({
  Link: 'ldm-link',            // a link, picture, video or sound
  Page: 'ldm-page',            // the page itself, e.g. a file the browser shows
  Selected: 'ldm-selected',
  All: 'ldm-all',
});

const WEB_PAGES = ['http://*/*', 'https://*/*'];

/**
 * The menu items to create, as contextMenus.create options. the reference's extension also adds "Transfer
 * download to the reference" to links in Chrome's downloads page; LDM leaves it out, because current Chrome
 * shows no link for a running download there, and a finished one links to the file on disk.
 * @param {{downloadLink: boolean, downloadAll: boolean}} menu
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
  return items;
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

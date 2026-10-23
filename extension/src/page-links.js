// Collects the links of a page for Download All Links and Download selected links (checklist
// B-04, B-05). collectPageLinks runs inside each frame of the page through scripting.executeScript,
// which sends only the function's source: it must not use anything from outside its own body.
// What it returns is page-controlled (page code can change the DOM it reads), so the background
// checks every entry again (protocol.js makeLinks) and LDM checks the message once more.

/**
 * The links of this frame, in page order: <a> and <area> links, pictures, frames, and addresses
 * written in the text and scripts of the page. With `selectionOnly`, only what the user selected:
 * links and pictures inside the selection and addresses in the selected text.
 * @param {boolean} selectionOnly
 * @param {number} maxLinks stops collecting after this many
 * @returns {{url: string, text: string, kind: string, fileName?: string}[]}
 */
export function collectPageLinks(selectionOnly, maxLinks) {
  const links = [];
  const add = (address, text, kind, fileName) => {
    if (links.length >= maxLinks || typeof address !== 'string' || address === '') {
      return;
    }
    let url;
    try {
      url = new URL(address, document.baseURI);
    } catch {
      return;
    }
    if (!/^(https?|ftps?):$/.test(url.protocol)) {
      return;
    }
    const entry = { url: url.href, text: String(text ?? '').slice(0, 2048), kind };
    if (fileName) {
      entry.fileName = String(fileName).slice(0, 2048);
    }
    links.push(entry);
  };
  // Absolute addresses written as text. Stops before quotes, brackets and white space, and drops
  // punctuation that usually ends the sentence rather than the address.
  const ADDRESS = /\b(?:https?|ftps?):\/\/[^\s"'<>()[\]{}\\^`|]+/gi;
  const addTextAddresses = (text) => {
    for (const match of String(text).matchAll(ADDRESS)) {
      add(match[0].replace(/[.,;:!?]+$/, ''), '', 'text');
    }
  };
  const linkText = (element) => (element.innerText || element.textContent || element.getAttribute('title') || '');
  const addElement = (element) => {
    if (element instanceof HTMLAnchorElement || element instanceof HTMLAreaElement) {
      if (element.hasAttribute('href')) {
        const image = element.querySelector?.('img[alt]');
        const text = linkText(element).trim() || element.getAttribute('alt') || image?.getAttribute('alt') || '';
        add(element.href, text, 'link', element.getAttribute('download') || undefined);
      }
    } else if (element instanceof HTMLImageElement) {
      add(element.currentSrc || element.src, element.alt, 'image');
    } else if (element instanceof HTMLIFrameElement || element instanceof HTMLFrameElement) {
      add(element.src, element.title, 'frame');
    }
  };
  const SELECTOR = 'a[href], area[href], img, iframe[src], frame[src]';

  if (selectionOnly) {
    const selection = getSelection();
    if (!selection) {
      return links;
    }
    for (let i = 0; i < selection.rangeCount; i += 1) {
      const range = selection.getRangeAt(i);
      const root = range.commonAncestorContainer;
      const container = root.nodeType === Node.ELEMENT_NODE ? root : root.parentElement;
      // A selection inside one link: the link itself.
      const enclosing = container?.closest?.('a[href], area[href]');
      if (enclosing) {
        addElement(enclosing);
      }
      for (const element of container?.querySelectorAll?.(SELECTOR) ?? []) {
        if (element !== enclosing && range.intersectsNode(element)) {
          addElement(element);
        }
      }
    }
    addTextAddresses(selection.toString());
    return links;
  }

  for (const element of document.querySelectorAll(SELECTOR)) {
    addElement(element);
  }
  if (document.body) {
    addTextAddresses(document.body.innerText || '');
  }
  for (const script of document.scripts) {
    if (!script.src) {
      addTextAddresses(script.textContent || '');
    }
  }
  return links;
}

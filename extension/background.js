// Motiva browser extension - service worker.
//
// Talks to the Motiva desktop app through Chrome Native Messaging: the
// browser starts MotivaBrowserHost.exe (registered by Motiva's Settings),
// which relays each JSON message to the running Motiva window and relays the
// replies back. Every request carries an `id`; the matching reply echoes it.
//
// Getting media to Motiva ("both, with fallback"):
//   1. The browser fetches the file itself (it has the user's cookies and
//      logins, so it works on more sites) and streams it to Motiva in chunks.
//   2. If that fails, Motiva is given the URL and downloads it itself.
// Either way Motiva stores the file in its own cache folder and uses that
// cached file for the in-app preview, playlists and the desktop wallpaper.

const HOST_NAME = 'com.motiva.browser';
const CHUNK_BYTES = 384 * 1024;
const MAX_BYTES = 2 * 1024 * 1024 * 1024; // matches the limit enforced by Motiva
const CALL_TIMEOUT_MS = 10 * 60 * 1000; // a download inside Motiva can take a while

let port = null;
let nextId = 1;
const pending = new Map();

function connectHost() {
  if (port) return port;
  const p = chrome.runtime.connectNative(HOST_NAME);
  p.onMessage.addListener((msg) => {
    const entry = pending.get(msg && msg.id);
    if (entry) {
      pending.delete(msg.id);
      clearTimeout(entry.timer);
      entry.resolve(msg);
    }
  });
  p.onDisconnect.addListener(() => {
    const reason = (chrome.runtime.lastError && chrome.runtime.lastError.message) || 'Disconnected';
    if (port === p) port = null;
    for (const [id, entry] of pending) {
      clearTimeout(entry.timer);
      entry.reject(new Error(reason));
      pending.delete(id);
    }
  });
  port = p;
  return p;
}

function call(message, timeoutMs = 30000) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error('Motiva did not answer in time'));
    }, Math.min(timeoutMs, CALL_TIMEOUT_MS));
    pending.set(id, { resolve, reject, timer });
    try {
      connectHost().postMessage({ ...message, id });
    } catch (e) {
      pending.delete(id);
      clearTimeout(timer);
      reject(e);
    }
  });
}

// ---------------------------------------------------------------- helpers

function toast(tabId, text, level) {
  if (tabId == null || tabId < 0) return;
  chrome.tabs.sendMessage(tabId, { type: 'toast', text, level: level || 'info' }, { frameId: 0 }).catch(() => {});
}

function friendlyError(e) {
  const m = (e && e.message) || String(e);
  if (/native messaging host not found|Specified native messaging host not found/i.test(m)) {
    return 'Motiva helper is not installed. Turn on "Browser extension" in Motiva > Settings > Integration.';
  }
  if (/Access to the specified native messaging host is forbidden/i.test(m)) {
    return 'Motiva helper does not allow this extension. Re-enable the browser extension in Motiva settings.';
  }
  return m;
}

function bytesToBase64(bytes) {
  let binary = '';
  for (let i = 0; i < bytes.length; i += 0x8000) {
    binary += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  }
  return btoa(binary);
}

function newUploadId() {
  const a = new Uint8Array(12);
  crypto.getRandomValues(a);
  return Array.from(a, (b) => b.toString(16).padStart(2, '0')).join('');
}

function nameFromUrl(url) {
  try {
    const last = new URL(url).pathname.split('/').filter(Boolean).pop() || '';
    return decodeURIComponent(last);
  } catch (e) {
    return '';
  }
}

async function sendChunk(uploadId, seq, bytes) {
  const reply = await call({ type: 'chunk', uploadId, seq, data: bytesToBase64(bytes) });
  if (!reply.ok) throw new Error(reply.error || 'Motiva rejected the data');
}

// Step 1 of the fallback chain: the browser downloads the file and streams it
// to Motiva. Returns { uploadId, mime, name } or throws.
async function uploadViaBrowser(src) {
  const resp = await fetch(src, { credentials: 'include', cache: 'force-cache' });
  if (!resp.ok) throw new Error('HTTP ' + resp.status);
  const mime = (resp.headers.get('content-type') || '').split(';')[0].trim().toLowerCase();
  if (/mpegurl|dash\+xml|^text\//.test(mime)) {
    throw new Error('This is a streaming or web-page address, not a media file');
  }
  const length = Number(resp.headers.get('content-length') || 0);
  if (length > MAX_BYTES) throw new Error('File is larger than 2 GB');

  const uploadId = newUploadId();
  const reader = resp.body.getReader();
  let pendingParts = [];
  let pendingSize = 0;
  let seq = 0;
  let total = 0;

  const flush = async () => {
    if (!pendingSize) return;
    const merged = new Uint8Array(pendingSize);
    let offset = 0;
    for (const part of pendingParts) {
      merged.set(part, offset);
      offset += part.length;
    }
    pendingParts = [];
    pendingSize = 0;
    await sendChunk(uploadId, seq++, merged);
  };

  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.length;
      if (total > MAX_BYTES) throw new Error('File is larger than 2 GB');
      pendingParts.push(value);
      pendingSize += value.length;
      if (pendingSize >= CHUNK_BYTES) await flush();
    }
    await flush();
  } catch (e) {
    reader.cancel().catch(() => {});
    call({ type: 'abort', uploadId }).catch(() => {});
    throw e;
  }
  if (total === 0) {
    call({ type: 'abort', uploadId }).catch(() => {});
    throw new Error('The file was empty');
  }
  return { uploadId, mime, name: nameFromUrl(src) };
}

// blob:/data: addresses only exist inside the page that made them, so that
// page's content script reads them and relays the chunks through us.
async function uploadViaPage(tabId, frameId, src) {
  const uploadId = newUploadId();
  const reply = await chrome.tabs.sendMessage(tabId, { type: 'uploadLocalUrl', url: src, uploadId }, { frameId });
  if (!reply || !reply.ok) {
    call({ type: 'abort', uploadId }).catch(() => {});
    throw new Error((reply && reply.error) || 'The page did not return the file');
  }
  return { uploadId, mime: reply.mime || '', name: reply.name || '' };
}

async function sendMedia({ action, playlistId, src, pageUrl, kind, tabId, frameId }) {
  const labels = { wallpaper: 'Set as wallpaper', playlist: 'Add to playlist', cache: 'Save to cache' };
  toast(tabId, 'Motiva: sending… (' + labels[action] + ')');
  try {
    let upload = null;
    if (/^(blob|data):/i.test(src)) {
      upload = await uploadViaPage(tabId, frameId || 0, src);
    } else if (/^https?:/i.test(src)) {
      try {
        upload = await uploadViaBrowser(src);
      } catch (e) {
        console.warn('Browser fetch failed, asking Motiva to download instead:', e);
      }
    } else {
      throw new Error('Unsupported address type');
    }
    const reply = await call(
      {
        type: 'media',
        action,
        playlistId: playlistId || 0,
        url: /^https?:/i.test(src) ? src : '',
        pageUrl: pageUrl || '',
        kind: kind || '',
        uploadId: upload ? upload.uploadId : '',
        mime: upload ? upload.mime : '',
        name: upload ? upload.name : nameFromUrl(src),
      },
      CALL_TIMEOUT_MS
    );
    if (!reply.ok) throw new Error(reply.error || 'Motiva could not use this file');
    toast(tabId, reply.message || 'Motiva: done', 'ok');
    refreshPlaylists();
  } catch (e) {
    toast(tabId, 'Motiva: ' + friendlyError(e), 'error');
  }
}

// ------------------------------------------------------------ playlists

let lastPlaylistRefresh = 0;
let menuBuild = Promise.resolve();

async function storedPlaylists() {
  const data = await chrome.storage.local.get('playlists');
  return Array.isArray(data.playlists) ? data.playlists : [];
}

async function refreshPlaylists(force) {
  const now = Date.now();
  if (!force && now - lastPlaylistRefresh < 30000) return;
  lastPlaylistRefresh = now;
  try {
    const reply = await call({ type: 'playlists' }, 8000);
    if (reply.ok && Array.isArray(reply.playlists)) {
      const before = JSON.stringify(await storedPlaylists());
      if (before !== JSON.stringify(reply.playlists)) {
        await chrome.storage.local.set({ playlists: reply.playlists });
        rebuildMenus();
      }
    }
    // "not running": keep the last known list; Motiva is started on demand.
  } catch (e) {
    // Helper not installed yet - nothing to refresh.
  }
}

function rebuildMenus() {
  menuBuild = menuBuild.then(async () => {
    const playlists = await storedPlaylists();
    await chrome.contextMenus.removeAll();
    const media = ['image', 'video'];
    chrome.contextMenus.create({ id: 'motiva', title: 'Motiva', contexts: media });
    chrome.contextMenus.create({ id: 'wallpaper', parentId: 'motiva', title: 'Set as wallpaper', contexts: media });
    chrome.contextMenus.create({ id: 'playlist', parentId: 'motiva', title: 'Add to playlist', contexts: media });
    for (const pl of playlists) {
      const contexts = [];
      if (pl.type === 'image' || pl.type === 'mixed') contexts.push('image');
      if (pl.type === 'video' || pl.type === 'mixed') contexts.push('video');
      if (!contexts.length) continue;
      chrome.contextMenus.create({
        id: 'pl:' + pl.id,
        parentId: 'playlist',
        title: pl.name || 'Playlist ' + pl.id,
        contexts,
      });
    }
    chrome.contextMenus.create({ id: 'pl:new', parentId: 'playlist', title: 'New playlist…', contexts: media });
    chrome.contextMenus.create({ id: 'cache', parentId: 'motiva', title: 'Save to Motiva cache', contexts: media });
  }).catch((e) => console.warn('Menu build failed', e));
  return menuBuild;
}

chrome.contextMenus.onClicked.addListener((info, tab) => {
  const id = String(info.menuItemId);
  const src = info.srcUrl || info.linkUrl;
  if (!src || !tab) return;
  const args = {
    src,
    pageUrl: info.frameUrl || info.pageUrl || tab.url || '',
    kind: info.mediaType === 'video' ? 'video' : 'image',
    tabId: tab.id,
    frameId: info.frameId || 0,
  };
  if (id === 'wallpaper') sendMedia({ ...args, action: 'wallpaper' });
  else if (id === 'cache') sendMedia({ ...args, action: 'cache' });
  else if (id === 'pl:new') sendMedia({ ...args, action: 'playlist', playlistId: 0 });
  else if (id.startsWith('pl:')) sendMedia({ ...args, action: 'playlist', playlistId: Number(id.slice(3)) });
});

// -------------------------------------------------------- page messages

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (!msg || !msg.type) return false;
  if (msg.type === 'getPlaylists') {
    storedPlaylists().then((playlists) => sendResponse({ playlists }));
    refreshPlaylists();
    return true;
  }
  if (msg.type === 'send') {
    sendMedia({
      action: msg.action,
      playlistId: msg.playlistId,
      src: msg.src,
      pageUrl: msg.pageUrl,
      kind: msg.kind,
      tabId: sender.tab && sender.tab.id,
      frameId: sender.frameId,
    });
    sendResponse({ ok: true });
    return false;
  }
  if (msg.type === 'relayChunk') {
    sendChunk(msg.uploadId, msg.seq, Uint8Array.from(atob(msg.data), (c) => c.charCodeAt(0)))
      .then(() => sendResponse({ ok: true }))
      .catch((e) => sendResponse({ ok: false, error: e.message }));
    return true;
  }
  if (msg.type === 'status') {
    call({ type: 'ping' }, 5000)
      .then((r) => sendResponse({ connected: !!r.ok, appRunning: !!r.appRunning, version: r.version || '' }))
      .catch((e) => sendResponse({ connected: false, error: friendlyError(e) }));
    return true;
  }
  return false;
});

chrome.runtime.onInstalled.addListener(() => {
  rebuildMenus().then(() => refreshPlaylists(true));
});
chrome.runtime.onStartup.addListener(() => {
  rebuildMenus().then(() => refreshPlaylists(true));
});
chrome.tabs.onActivated.addListener(() => refreshPlaylists());

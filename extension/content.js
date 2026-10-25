// Motiva browser extension - content script.
// Shows a small Motiva button over the image/video under the pointer and
// reads blob:/data: media for the background script. Everything visible lives
// in a closed shadow root so page styles cannot break it (and it cannot be
// read by the page).

(() => {
  if (window.top !== window && window.innerWidth < 160) return; // tiny ad frames

  const MIN_SIZE = 120; // px - skip icons, avatars, tracking pixels
  const CHUNK = 384 * 1024;

  let overlayEnabled = true;
  try {
    chrome.storage.local.get('overlay').then((d) => {
      overlayEnabled = d.overlay !== false;
    });
    chrome.storage.onChanged.addListener((changes) => {
      if (changes.overlay) overlayEnabled = changes.overlay.newValue !== false;
    });
  } catch (e) {}

  // ----------------------------------------------------------------- UI
  const host = document.createElement('div');
  host.style.cssText = 'all:initial;position:fixed;left:0;top:0;width:0;height:0;z-index:2147483647;';
  const root = host.attachShadow({ mode: 'closed' });
  root.innerHTML = `
    <style>
      :host{all:initial}
      .btn{position:fixed;display:none;align-items:center;gap:6px;padding:5px 10px 5px 8px;border-radius:999px;
        background:rgba(18,18,24,.88);color:#fff;font:600 12px/1 system-ui,Segoe UI,sans-serif;cursor:pointer;
        border:1px solid rgba(255,255,255,.22);box-shadow:0 2px 10px rgba(0,0,0,.45);user-select:none;backdrop-filter:blur(6px)}
      .btn:hover{background:rgba(52,92,255,.95)}
      .dot{width:8px;height:8px;border-radius:50%;background:#5b8cff}
      .menu{position:fixed;display:none;min-width:190px;max-width:260px;max-height:320px;overflow:auto;padding:4px;border-radius:10px;
        background:rgba(24,24,32,.97);color:#f2f2f6;font:13px/1.3 system-ui,Segoe UI,sans-serif;
        border:1px solid rgba(255,255,255,.16);box-shadow:0 8px 28px rgba(0,0,0,.55)}
      .item{padding:7px 10px;border-radius:6px;cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
      .item:hover{background:rgba(91,140,255,.35)}
      .sep{height:1px;margin:4px 6px;background:rgba(255,255,255,.14)}
      .head{padding:5px 10px;font-size:11px;opacity:.6;text-transform:uppercase;letter-spacing:.04em}
      .toast{position:fixed;right:18px;bottom:18px;max-width:340px;padding:10px 14px;border-radius:10px;display:none;
        background:rgba(24,24,32,.96);color:#fff;font:13px/1.35 system-ui,Segoe UI,sans-serif;border:1px solid rgba(255,255,255,.18);
        box-shadow:0 6px 24px rgba(0,0,0,.5)}
      .toast.ok{border-color:#3fbf7f}.toast.error{border-color:#ef5b5b}
    </style>
    <div class="btn" id="btn"><span class="dot"></span>Motiva</div>
    <div class="menu" id="menu"></div>
    <div class="toast" id="toast"></div>`;
  const btn = root.getElementById('btn');
  const menu = root.getElementById('menu');
  const toastEl = root.getElementById('toast');
  let toastTimer = 0;

  function mount() {
    if (!host.isConnected) (document.body || document.documentElement).appendChild(host);
  }

  function showToast(text, level) {
    mount();
    toastEl.textContent = text;
    toastEl.className = 'toast ' + (level || '');
    toastEl.style.display = 'block';
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => (toastEl.style.display = 'none'), level === 'error' ? 7000 : 3500);
  }

  // ------------------------------------------------- finding the media
  let current = null; // { el, src, kind }

  function mediaSource(el) {
    if (el instanceof HTMLImageElement) return el.currentSrc || el.src || '';
    if (el instanceof HTMLVideoElement) {
      return el.currentSrc || el.src || (el.querySelector('source[src]') || {}).src || '';
    }
    return '';
  }

  function eligible(el) {
    if (!(el instanceof HTMLImageElement || el instanceof HTMLVideoElement)) return null;
    const r = el.getBoundingClientRect();
    if (r.width < MIN_SIZE || r.height < MIN_SIZE) return null;
    const src = mediaSource(el);
    if (!/^(https?|blob|data):/i.test(src)) return null;
    if (/^data:image\/svg/i.test(src)) return null;
    return { el, src, kind: el instanceof HTMLVideoElement ? 'video' : 'image', rect: r };
  }

  function findUnder(x, y) {
    // elementsFromPoint sees through transparent overlay layers that many
    // sites put on top of their pictures.
    for (const el of document.elementsFromPoint(x, y)) {
      if (el === host) return null;
      const hit = eligible(el);
      if (hit) return hit;
    }
    return null;
  }

  function placeButton(hit) {
    mount();
    const r = hit.rect;
    const left = Math.max(4, Math.min(window.innerWidth - 90, r.left + 8));
    const top = Math.max(4, Math.min(window.innerHeight - 34, r.top + 8));
    btn.style.left = left + 'px';
    btn.style.top = top + 'px';
    btn.style.display = 'flex';
  }

  let lastMove = 0;
  document.addEventListener(
    'mousemove',
    (e) => {
      if (!overlayEnabled || menu.style.display === 'block') return;
      const now = performance.now();
      if (now - lastMove < 80) return;
      lastMove = now;
      const path = e.composedPath();
      if (path.includes(host)) return;
      const hit = findUnder(e.clientX, e.clientY);
      if (hit) {
        current = hit;
        placeButton(hit);
      } else {
        current = null;
        btn.style.display = 'none';
      }
    },
    { passive: true, capture: true }
  );
  document.addEventListener('scroll', () => {
    btn.style.display = 'none';
    menu.style.display = 'none';
  }, { passive: true, capture: true });

  // ------------------------------------------------------------- menu
  function send(action, playlistId) {
    if (!current) return;
    menu.style.display = 'none';
    chrome.runtime.sendMessage({
      type: 'send',
      action,
      playlistId: playlistId || 0,
      src: current.src,
      pageUrl: location.href,
      kind: current.kind,
    });
  }

  function addItem(label, onClick, cls) {
    const d = document.createElement('div');
    d.className = cls || 'item';
    d.textContent = label;
    if (onClick) d.addEventListener('click', onClick);
    menu.appendChild(d);
  }

  async function openMenu() {
    if (!current) return;
    const kind = current.kind;
    let playlists = [];
    try {
      playlists = ((await chrome.runtime.sendMessage({ type: 'getPlaylists' })) || {}).playlists || [];
    } catch (e) {}
    menu.textContent = '';
    addItem('Set as wallpaper', () => send('wallpaper'));
    addItem('Save to Motiva cache', () => send('cache'));
    const sep = document.createElement('div');
    sep.className = 'sep';
    menu.appendChild(sep);
    addItem('Add to playlist', null, 'head');
    for (const pl of playlists) {
      if (pl.type === 'mixed' || pl.type === kind) addItem(pl.name, () => send('playlist', pl.id));
    }
    addItem('New playlist…', () => send('playlist', 0));
    const r = btn.getBoundingClientRect();
    menu.style.left = Math.min(r.left, window.innerWidth - 270) + 'px';
    menu.style.top = r.bottom + 6 + 'px';
    menu.style.display = 'block';
  }

  btn.addEventListener('click', (e) => {
    e.stopPropagation();
    e.preventDefault();
    if (menu.style.display === 'block') menu.style.display = 'none';
    else openMenu();
  });
  document.addEventListener('mousedown', (e) => {
    if (!e.composedPath().includes(host)) menu.style.display = 'none';
  }, true);
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') menu.style.display = 'none';
  }, true);

  // ------------------------------------------------ messages from the worker
  chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
    if (msg.type === 'toast') {
      showToast(msg.text, msg.level);
      return false;
    }
    if (msg.type === 'uploadLocalUrl') {
      (async () => {
        try {
          const resp = await fetch(msg.url);
          const blob = await resp.blob();
          if (!blob.size) throw new Error('The file was empty');
          if (blob.size > 2 * 1024 * 1024 * 1024) throw new Error('File is larger than 2 GB');
          let seq = 0;
          for (let off = 0; off < blob.size; off += CHUNK) {
            const bytes = new Uint8Array(await blob.slice(off, off + CHUNK).arrayBuffer());
            let bin = '';
            for (let i = 0; i < bytes.length; i += 0x8000) {
              bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
            }
            const r = await chrome.runtime.sendMessage({
              type: 'relayChunk',
              uploadId: msg.uploadId,
              seq: seq++,
              data: btoa(bin),
            });
            if (!r || !r.ok) throw new Error((r && r.error) || 'Motiva rejected the data');
          }
          sendResponse({ ok: true, mime: blob.type || '', name: '' });
        } catch (e) {
          sendResponse({ ok: false, error: e.message });
        }
      })();
      return true;
    }
    return false;
  });
})();

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
  let compatEnabled = true; // "Resolution-Based Compatibility", default on
  let compatDebug = false; // chrome.storage.local.debugCompat = true -> console trace
  try {
    chrome.storage.local.get(['overlay', 'resolutionCompat', 'debugCompat']).then((d) => {
      overlayEnabled = d.overlay !== false;
      compatEnabled = d.resolutionCompat !== false;
      compatDebug = d.debugCompat === true;
    });
    chrome.storage.onChanged.addListener((changes) => {
      if (changes.debugCompat) compatDebug = changes.debugCompat.newValue === true;
      if (changes.overlay) overlayEnabled = changes.overlay.newValue !== false;
      if (changes.resolutionCompat) {
        compatEnabled = changes.resolutionCompat.newValue !== false;
        reportContextMenu(true);
        refreshButton();
      }
    });
  } catch (e) {}

  // Physical pixel size of the display this window is on. screen.* is in
  // CSS px, so multiplying by devicePixelRatio gives device pixels; read
  // fresh on every check, so moving the window to another monitor or
  // changing the desktop resolution is picked up without a reload.
  function displaySize() {
    const dpr = window.devicePixelRatio || 1;
    return { width: Math.round(screen.width * dpr), height: Math.round(screen.height * dpr) };
  }

  // Intrinsic size from metadata the browser already loaded: no decoding,
  // no extra requests. 0 until the image/video metadata is available.
  function mediaSize(el) {
    if (el instanceof HTMLVideoElement) return { width: el.videoWidth, height: el.videoHeight };
    return { width: el.naturalWidth, height: el.naturalHeight };
  }

  // visible = compatEnabled ? compatible : previous behaviour (always true).
  // Unknown size (metadata not loaded / invalid) is not shown; the load and
  // loadedmetadata listeners below re-run this when it becomes known.
  let lastTrace = '';
  function isCompatible(el) {
    const media = mediaSize(el);
    const display = displaySize();
    const d = MotivaCompat.decide(compatEnabled, media, display);
    if (compatDebug) {
      const line = `${d.state}|${media.width}x${media.height}|${display.width}x${display.height}`;
      if (line !== lastTrace) {
        lastTrace = line;
        console.debug('[Motiva compat]', d.state, {
          media, display, dpr: window.devicePixelRatio, screen: [screen.width, screen.height],
          enabled: compatEnabled, reason: d.result && d.result.reason, src: mediaSource(el).slice(0, 80),
        });
      }
    }
    return d.visible;
  }

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

  // Button visibility is a pure function of the media under the pointer:
  // visible <=> eligible media there && (compat off || compatible). It is
  // re-derived on every pointer move (throttled, with a trailing run so the
  // last position is never skipped) and whenever the media under a still
  // pointer changes or finishes loading, so no stale state survives.
  let lastMove = 0;
  let lastX = -1;
  let lastY = -1;
  let trailTimer = 0;
  let overHost = false;

  // After the extension is reloaded, tabs keep an orphaned copy of this
  // script whose chrome.* calls throw; it must go quiet instead of erroring.
  function alive() {
    try {
      return !!chrome.runtime.id;
    } catch (e) {
      return false;
    }
  }

  function syncButton() {
    trailTimer = 0;
    if (!alive()) {
      btn.style.display = 'none';
      return;
    }
    if (!overlayEnabled || menu.style.display === 'block' || overHost || lastX < 0) return;
    const hit = findUnder(lastX, lastY);
    const ok = !hit || isCompatible(hit.el);
    reportContextMenu(ok);
    if (hit && ok) {
      current = hit;
      placeButton(hit);
    } else {
      current = null;
      btn.style.display = 'none';
    }
  }

  document.addEventListener(
    'mousemove',
    (e) => {
      if (!alive()) return;
      overHost = e.composedPath().includes(host);
      if (overHost) return;
      lastX = e.clientX;
      lastY = e.clientY;
      const now = performance.now();
      if (now - lastMove < 80) {
        if (!trailTimer) trailTimer = setTimeout(syncButton, 90);
        return;
      }
      lastMove = now;
      syncButton();
    },
    { passive: true, capture: true }
  );
  // Image/video metadata arriving or a carousel swapping the source under a
  // stationary pointer changes the answer without any mouse movement.
  for (const type of ['load', 'loadedmetadata', 'emptied']) {
    document.addEventListener(type, (e) => {
      if (e.target === (current && current.el) || e.target instanceof HTMLImageElement || e.target instanceof HTMLVideoElement) {
        if (!trailTimer) trailTimer = setTimeout(syncButton, 0);
      }
    }, { passive: true, capture: true });
  }
  // Right-click "Motiva" entry follows the same result as the hover button.
  // Reported ahead of time (on hover, only when it changes) so the menu is
  // already correct when the user right-clicks; reset when the pointer leaves
  // this frame so another frame's stale answer is never reused.
  let lastCtx = null;
  function reportContextMenu(ok) {
    if (!compatEnabled) ok = true;
    if (ok === lastCtx) return;
    lastCtx = ok;
    try {
      chrome.runtime.sendMessage({ type: 'ctxCompat', compatible: ok }).catch(() => {});
    } catch (err) {}
  }
  document.documentElement.addEventListener('mouseleave', () => (lastCtx = null), { passive: true });
  document.addEventListener(
    'contextmenu',
    (e) => {
      lastCtx = null; // always resend: the target is known exactly now
      const hit = findUnder(e.clientX, e.clientY);
      const el = hit ? hit.el : e.composedPath()[0];
      const media = el instanceof HTMLImageElement || el instanceof HTMLVideoElement;
      reportContextMenu(!media || isCompatible(el));
    },
    { capture: true, passive: true }
  );

  // Re-check the media the button is on after a setting or display change.
  function refreshButton() {
    if (current && !isCompatible(current.el)) menu.style.display = 'none';
    syncButton();
  }
  window.addEventListener('resize', refreshButton, { passive: true });
  try {
    window.matchMedia(`(resolution: ${window.devicePixelRatio}dppx)`).addEventListener('change', refreshButton);
  } catch (e) {}

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

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
      if (changes.overlay) {
        overlayEnabled = changes.overlay.newValue !== false;
        syncButton(); // disabled -> hides immediately; enabled -> re-derives
      }
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

  // Best-known source size of an <img>, with no decoding and no requests:
  // the decoded natural size, or a larger candidate the page itself declares
  // in srcset ('w' = width, 'x' = density x rendered width; aspect ratio
  // from the natural size, else the rendered box). URL words like "original"
  // prove nothing and are ignored. Parsed once per distinct srcset string.
  const srcsetCache = new WeakMap(); // el -> { str, list: [{ w?, x? }] }
  function parseSrcset(el) {
    const str = el.srcset || '';
    const hit = srcsetCache.get(el);
    if (hit && hit.str === str) return hit.list;
    const list = [];
    for (const part of str.split(',')) {
      const bits = part.trim().split(/\s+/);
      if (!bits[0] || bits.length > 2) continue;
      const m = bits[1] && /^(\d+(?:\.\d+)?)([wx])$/i.exec(bits[1]);
      if (!m) continue;
      const n = parseFloat(m[1]);
      if (!(n > 0) || !isFinite(n)) continue;
      list.push(m[2].toLowerCase() === 'w' ? { w: n } : { x: n });
    }
    srcsetCache.set(el, { str, list });
    return list;
  }

  function sourceSize(el) {
    if (!(el instanceof HTMLImageElement)) return mediaSize(el);
    const nat = mediaSize(el);
    const natOk = nat.width > 0 && nat.height > 0;
    const box = el.getBoundingClientRect();
    const ar = natOk ? nat.width / nat.height : box.height > 0 ? box.width / box.height : 0;
    let best = natOk ? nat : { width: 0, height: 0 };
    if (!(ar > 0)) return best;
    for (const c of parseSrcset(el)) {
      const width = c.w || (box.width > 0 ? box.width * c.x : 0);
      if (width > best.width) best = { width, height: width / ar };
    }
    return best;
  }

  // Three outcomes, never a silent "compatible":
  //   visible && !lowRes  compatible source
  //   visible && lowRes   best-known image source fails the unchanged
  //                       thresholds: shown with a low-resolution marker
  //   !visible            unknown size (not invented) or incompatible video
  let lastTrace = '';
  function assess(el) {
    const media = sourceSize(el);
    const display = displaySize();
    const d = MotivaCompat.decide(compatEnabled, media, display);
    if (compatDebug) {
      const nat = mediaSize(el);
      const line = `${d.state}|${nat.width}x${nat.height}|${media.width}x${media.height}|${display.width}x${display.height}`;
      if (line !== lastTrace) {
        lastTrace = line;
        console.debug('[Motiva compat]', d.state, {
          natural: nat, best: media, display, dpr: window.devicePixelRatio, screen: [screen.width, screen.height],
          enabled: compatEnabled, reason: d.result && d.result.reason, src: mediaSource(el).slice(0, 80),
        });
      }
    }
    const lowRes = d.state === 'incompatible' && el instanceof HTMLImageElement;
    return { visible: d.visible || lowRes, lowRes };
  }
  function isCompatible(el) {
    return assess(el).visible;
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
      .btn.low .dot{background:#f5a524}.btn.low::after{content:'low-res';font-weight:500;opacity:.75;font-size:11px}
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
    btn.className = hit.lowRes ? 'btn low' : 'btn';
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

  // syncButton() is the single owner of button visibility; every branch ends
  // in an explicit state: shown on `current`, or hidden with current = null.
  function hideAll() {
    btn.style.display = 'none';
    menu.style.display = 'none';
    current = null;
  }

  // The media a menu/button action would act on is still the same live,
  // compatible element it was when the menu opened.
  function currentValid() {
    const c = current;
    return !!(c && c.el.isConnected && mediaSource(c.el) === c.src && isCompatible(c.el));
  }

  // The single owner of trailTimer: every timer fires through here, so the
  // handle is cleared exactly when its timer has run. syncButton() never
  // touches trailTimer, so a direct call cannot orphan a pending timer.
  function runTrail() {
    trailTimer = 0;
    lastMove = performance.now();
    syncButton();
  }

  function syncButton() {
    if (!alive() || !overlayEnabled || lastX < 0) {
      hideAll();
      return;
    }
    if (menu.style.display === 'block') {
      // Menu open: keep it (and its media) only while that media is valid.
      if (currentValid()) return;
      hideAll();
    } else if (overHost) {
      // Pointer is on the button itself: keep it, but never for dead media.
      if (currentValid()) return;
      hideAll();
    }
    const hit = findUnder(lastX, lastY);
    const a = hit ? assess(hit.el) : null;
    const ok = !a || a.visible;
    reportContextMenu(ok);
    if (hit && ok) {
      hit.lowRes = a.lowRes;
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
        // Trailing edge: one timer, due when the current window ends.
        if (!trailTimer) trailTimer = setTimeout(runTrail, lastMove + 80 - now);
        return;
      }
      // Leading edge: this sync reads the latest coordinates, so any pending
      // timer is redundant and is cleared rather than left to run again.
      if (trailTimer) {
        clearTimeout(trailTimer);
        trailTimer = 0;
      }
      lastMove = now;
      syncButton();
    },
    { passive: true, capture: true }
  );
  // Image/video metadata arriving or a carousel swapping the source under a
  // stationary pointer changes the answer without any mouse movement.
  // 'resize' (capture only, it does not bubble) fires on a <video> whose
  // intrinsic dimensions change.
  function scheduleSync(delay) {
    if (!trailTimer) trailTimer = setTimeout(runTrail, delay);
  }
  for (const type of ['load', 'loadedmetadata', 'emptied', 'resize']) {
    document.addEventListener(type, (e) => {
      if (e.target === (current && current.el) || e.target instanceof HTMLImageElement || e.target instanceof HTMLVideoElement) {
        scheduleSync(0);
      }
    }, { passive: true, capture: true });
  }

  // DOM changes under a stationary pointer: media inserted/removed, or its
  // source swapped (lazy loading, carousels, SPAs). Everything is funnelled
  // into the same debounced syncButton(), so bursts cost one evaluation and
  // metadata that is not there yet is picked up by the load listeners above.
  // Mutations caused by the extension's own host element are ignored, so
  // mounting the button can never retrigger the observer.
  function isMediaNode(n) {
    if (n.nodeType !== 1 || n === host) return false;
    if (n instanceof HTMLImageElement || n instanceof HTMLVideoElement) return true;
    return !!(n.querySelector && n.querySelector('img,video'));
  }
  const observer = new MutationObserver((records) => {
    if (!alive()) return stopObserving();
    if (lastX < 0) return;
    for (const m of records) {
      if (m.type === 'attributes') {
        if (m.target !== host && (m.target instanceof HTMLImageElement || m.target instanceof HTMLVideoElement)) {
          return scheduleSync(50);
        }
        continue;
      }
      for (const n of m.addedNodes) if (isMediaNode(n)) return scheduleSync(50);
      for (const n of m.removedNodes) if (isMediaNode(n)) return scheduleSync(50);
    }
  });
  let observing = false;
  function startObserving() {
    if (observing) return;
    observing = true;
    observer.observe(document.documentElement, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ['src', 'srcset', 'poster'],
    });
  }
  function stopObserving() {
    observing = false;
    observer.disconnect();
    clearTimeout(trailTimer);
    trailTimer = 0;
    clearTimeout(scrollTimer); // pending post-scroll sync must not run later
    scrollTimer = 0;
  }
  startObserving();
  // Initial page initialisation, and bfcache restore (pagehide disconnects).
  document.addEventListener('DOMContentLoaded', () => scheduleSync(0), { once: true });
  window.addEventListener('load', () => scheduleSync(0), { once: true });
  window.addEventListener('pagehide', stopObserving);
  window.addEventListener('pageshow', (e) => {
    if (e.persisted) {
      startObserving();
      scheduleSync(0);
    }
  });
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

  // Scrolling moves media under a stationary pointer: hide at once (the
  // button would be misplaced), then re-evaluate once scrolling settles.
  // One timer, restarted per event, so rapid scrolling costs one sync.
  let scrollTimer = 0;
  document.addEventListener('scroll', () => {
    btn.style.display = 'none';
    menu.style.display = 'none';
    current = null;
    overHost = false;
    clearTimeout(scrollTimer);
    scrollTimer = setTimeout(() => {
      scrollTimer = 0;
      syncButton();
    }, 120);
  }, { passive: true, capture: true });

  // Pointer left the page/frame (relatedTarget null; moving into our own
  // shadow host stays inside the document so it never fires this).
  function pointerLeft() {
    clearTimeout(trailTimer);
    clearTimeout(scrollTimer);
    trailTimer = scrollTimer = 0;
    lastX = lastY = -1;
    overHost = false;
    lastCtx = null;
    hideAll();
  }
  document.addEventListener('mouseout', (e) => {
    if (!e.relatedTarget) pointerLeft();
  }, { passive: true, capture: true });

  // ------------------------------------------------------------- menu
  function send(action, playlistId) {
    if (!current) return;
    menu.style.display = 'none';
    if (!currentValid()) {
      syncButton();
      return;
    }
    chrome.runtime.sendMessage({
      type: 'send',
      action,
      playlistId: playlistId || 0,
      src: current.src,
      pageUrl: location.href,
      kind: current.kind,
      lowRes: !!current.lowRes,
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

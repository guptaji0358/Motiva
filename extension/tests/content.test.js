// Run: node --test extension/tests
// Drives extension/content.js inside a minimal fake DOM (no jsdom needed):
// fake clock, fake elementsFromPoint, stub chrome.* APIs.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const compatSrc = fs.readFileSync(path.join(__dirname, '../compat.js'), 'utf8');
const contentSrc = fs.readFileSync(path.join(__dirname, '../content.js'), 'utf8');

function setup() {
  // ---- fake clock
  let now = 1000, seq = 0;
  const timers = new Map();
  const setTimeout_ = (fn, ms) => { const id = ++seq; timers.set(id, { fn, at: now + (ms || 0) }); return id; };
  const clearTimeout_ = (id) => timers.delete(id);
  const advance = (ms) => {
    const end = now + ms;
    for (;;) {
      let next = null;
      for (const [id, t] of timers) if (t.at <= end && (!next || t.at < next.t.at)) next = { id, t };
      if (!next) break;
      timers.delete(next.id);
      now = Math.max(now, next.t.at);
      next.t.fn();
    }
    now = end;
  };

  // ---- fake DOM
  const listeners = {};
  const on = (type, fn) => (listeners[type] = listeners[type] || []).push(fn);
  const fire = (type, ev) => (listeners[type] || []).forEach((f) => f(ev));
  const stub = () => ({ style: {}, addEventListener() {}, appendChild() {}, textContent: '',
    getBoundingClientRect: () => ({ left: 0, top: 0, bottom: 0, width: 0, height: 0 }) });
  const parts = { btn: stub(), menu: stub(), toast: stub() };
  parts.btn.style.display = 'none'; parts.menu.style.display = 'none';
  class HTMLImageElement { constructor(w, h, src) { Object.assign(this, { naturalWidth: w, naturalHeight: h, src, currentSrc: src, isConnected: true, nodeType: 1, rect: { left: 50, top: 50, width: 400, height: 300 } }); } getBoundingClientRect() { return this.rect; } }
  class HTMLVideoElement { constructor(w, h, src) { Object.assign(this, { videoWidth: w, videoHeight: h, src, currentSrc: src, isConnected: true, nodeType: 1, rect: { left: 50, top: 50, width: 400, height: 300 } }); } getBoundingClientRect() { return this.rect; } querySelector() { return null; } }
  const host = { isConnected: false, style: {}, attachShadow: () => ({ set innerHTML(v) {}, getElementById: (id) => parts[id === 'toast' ? 'toast' : id] }) };
  let stack = []; // elements under the pointer
  const documentElement = { addEventListener() {}, querySelector: () => null };
  const document = {
    createElement: () => host.__used ? stub() : ((host.__used = true), host),
    documentElement, body: { appendChild: () => { host.isConnected = true; } },
    addEventListener: on, elementsFromPoint: () => stack,
  };
  const calls = { sent: [] };
  const mos = [];
  const ctx = {
    HTMLImageElement, HTMLVideoElement, document,
    window: { top: null, innerWidth: 1200, innerHeight: 800, devicePixelRatio: 1, addEventListener: on, matchMedia: () => ({ addEventListener() {} }) },
    screen: { width: 1920, height: 1080 }, performance: { now: () => now },
    setTimeout: setTimeout_, clearTimeout: clearTimeout_, console,
    MutationObserver: class { constructor(cb) { mos.push(cb); } observe() {} disconnect() {} },
    chrome: {
      runtime: { id: 'x', sendMessage: (m) => { calls.sent.push(m); return Promise.resolve({}); }, onMessage: { addListener() {} } },
      storage: { local: { get: () => Promise.resolve({}) }, onChanged: { addListener: (f) => (ctx.__storageChanged = f) } },
    },
  };
  ctx.window.top = ctx.window;
  vm.createContext(ctx);
  vm.runInContext(compatSrc, ctx);
  vm.runInContext(contentSrc, ctx);
  const move = (x, y, over) => fire('mousemove', { clientX: x, clientY: y, composedPath: () => (over ? [host] : []) });
  return {
    parts, calls, advance, fire, ctx, host, mos,
    img: (w, h, src = 'https://a/b.jpg') => new HTMLImageElement(w, h, src),
    vid: (w, h, src = 'https://a/v.mp4') => new HTMLVideoElement(w, h, src),
    under: (...els) => (stack = els),
    move,
    visible: () => parts.btn.style.display === 'flex',
  };
}

const GOOD = [1920, 1080], BAD = [640, 360];

test('compatible image normal button, known low-res image marked low-res', () => {
  const t = setup();
  t.under(t.img(...GOOD)); t.move(100, 100);
  assert.equal(t.visible(), true);
  assert.equal(t.parts.btn.className, 'btn');
  t.advance(200); t.under(t.img(...BAD)); t.move(110, 110);
  assert.equal(t.visible(), true);
  assert.equal(t.parts.btn.className, 'btn low');
});

test('compatible video visible, incompatible video hidden', () => {
  const t = setup();
  t.under(t.vid(...GOOD)); t.move(100, 100);
  assert.equal(t.visible(), true);
  t.advance(200); t.under(t.vid(...BAD)); t.move(110, 110);
  assert.equal(t.visible(), false);
});

test('scroll hides, then re-evaluates under stationary pointer once settled', () => {
  const t = setup();
  t.under(t.img(...GOOD)); t.move(100, 100);
  assert.equal(t.visible(), true);
  t.under(t.img(...GOOD, ) ); // new media scrolled under pointer
  for (let i = 0; i < 10; i++) { t.fire('scroll', {}); t.advance(20); }
  assert.equal(t.visible(), false, 'hidden while scrolling');
  t.advance(130);
  assert.equal(t.visible(), true, 'shown after settle');
  // scrolled onto incompatible media / nothing: stays hidden
  t.fire('scroll', {}); t.under(t.img(0, 0)); t.advance(130);
  assert.equal(t.visible(), false, 'unknown size stays hidden');
  t.fire('scroll', {}); t.under(); t.advance(130);
  assert.equal(t.visible(), false);
});

test('rapid scrolling schedules a single sync', () => {
  const t = setup();
  t.under(t.img(...GOOD)); t.move(100, 100);
  let n = 0; const orig = t.ctx.document.elementsFromPoint;
  t.ctx.document.elementsFromPoint = (...a) => (n++, orig(...a));
  for (let i = 0; i < 50; i++) { t.fire('scroll', {}); t.advance(10); }
  t.advance(200);
  assert.equal(n, 1);
});

test('pointer exits the page: hidden, state cleared, no sync resurrects it', () => {
  const t = setup();
  t.under(t.img(...GOOD)); t.move(100, 100);
  t.fire('mouseout', { relatedTarget: null });
  assert.equal(t.visible(), false);
  t.advance(500);
  assert.equal(t.visible(), false);
  // moving to our own shadow host (relatedTarget set) does not hide
  t.move(100, 100); assert.equal(t.visible(), true);
  t.fire('mouseout', { relatedTarget: t.host });
  assert.equal(t.visible(), true);
});

test('pointer over our own button keeps it; button dropped when media is gone', () => {
  const t = setup();
  const im = t.img(...GOOD);
  t.under(im); t.move(100, 100);
  t.advance(200); t.move(60, 60, true);
  assert.equal(t.visible(), true);
  im.isConnected = false; t.under();
  t.ctx.__mo = t.mos[0]; t.mos[0]([{ type: 'childList', addedNodes: [], removedNodes: [{ nodeType: 1, querySelector: () => ({}) }] }]);
  t.advance(100);
  assert.equal(t.visible(), false);
});

test('overlay disabled while button visible hides immediately', () => {
  const t = setup();
  t.under(t.img(...GOOD)); t.move(100, 100);
  assert.equal(t.visible(), true);
  t.ctx.__storageChanged({ overlay: { newValue: false } });
  assert.equal(t.visible(), false);
  t.advance(200); t.move(101, 101);
  assert.equal(t.visible(), false);
  t.ctx.__storageChanged({ overlay: { newValue: true } });
  assert.equal(t.visible(), true);
});

test('menu open then media replaced/removed: menu closes, no stale action', () => {
  const t = setup();
  const im = t.img(...GOOD);
  t.under(im); t.move(100, 100);
  t.parts.menu.style.display = 'block';
  t.advance(200); t.move(500, 500, true); // pointer on menu, valid media: kept
  assert.equal(t.parts.menu.style.display, 'block');
  assert.equal(t.visible(), true);
  im.src = im.currentSrc = 'https://a/other.jpg'; // source swapped
  t.mos[0]([{ type: 'attributes', target: im }]);
  t.advance(100);
  assert.equal(t.parts.menu.style.display, 'none'); // menu (and its stale media) dropped
  assert.equal(t.visible(), true); // button re-derived for the new source under the pointer
  t.advance(200); t.under(); t.move(300, 300);
  assert.equal(t.visible(), false);
});

test('rapid mouse movement is throttled to bounded syncs, last position wins', () => {
  const t = setup();
  const a = t.img(...GOOD), b = t.img(0, 0);
  t.under(a); t.move(1, 1);
  t.under(b);
  for (let i = 0; i < 20; i++) { t.move(i, i); t.advance(5); }
  t.advance(200);
  assert.equal(t.visible(), false);
});

test('dynamically inserted media under a stationary pointer is picked up', () => {
  const t = setup();
  t.under(); t.move(100, 100);
  assert.equal(t.visible(), false);
  const im = t.img(...GOOD); t.under(im);
  t.mos[0]([{ type: 'childList', addedNodes: [im], removedNodes: [] }]);
  t.advance(100);
  assert.equal(t.visible(), true);
});

test('stopping observation during a pending scroll timer: no later sync runs', () => {
  const t = setup();
  t.under(t.img(...GOOD)); t.move(100, 100);
  assert.equal(t.visible(), true);
  let n = 0; const orig = t.ctx.document.elementsFromPoint;
  t.ctx.document.elementsFromPoint = (...a) => (n++, orig(...a));
  t.fire('scroll', {}); // schedules the 120ms re-sync timer
  t.fire('pagehide', {}); // stopObserving() must cancel it
  n = 0;
  t.advance(500);
  assert.equal(n, 0, 'no delayed sync after observation stopped');
  assert.equal(t.visible(), false, 'scroll-hidden button not resurrected');
});

// ---- source-aware compatibility (best-known size, low-resolution state)
const show = (t, el) => { t.under(el); t.move(100, 100); return [t.visible(), t.parts.btn.className]; };

test('srcset w candidate: thumbnail whose declared source passes -> normal button', () => {
  const t = setup(), im = t.img(300, 200);
  im.srcset = 'https://a/s.jpg 300w, https://a/l.jpg 2400w';
  assert.deepEqual(show(t, im), [true, 'btn']);
});

test('srcset x descriptors use rendered width x density', () => {
  const t = setup(), im = t.img(400, 300); // rendered 400x300
  im.srcset = 'https://a/1.jpg 1x, https://a/5.jpg 5x'; // 2000x1500 -> passes
  assert.deepEqual(show(t, im), [true, 'btn']);
  const t2 = setup(), im2 = t2.img(400, 300);
  im2.srcset = 'https://a/1.jpg 1x, https://a/2.jpg 2x'; // 800 wide -> low-res
  assert.deepEqual(show(t2, im2), [true, 'btn low']);
});

test('malformed srcset is ignored, falls back to natural size', () => {
  const t = setup(), im = t.img(...BAD);
  im.srcset = ',, a.jpg 0w, b.jpg -5w, c.jpg abc, d.jpg 1e9q, e.jpg 100w 2x, f.jpg NaNx';
  assert.deepEqual(show(t, im), [true, 'btn low']);
});

test('best-known source still too small -> low-res, not compatible', () => {
  const t = setup(), im = t.img(300, 200);
  im.srcset = 'https://a/m.jpg 800w';
  assert.deepEqual(show(t, im), [true, 'btn low']);
});

test('unknown dimensions stay hidden; no srcset invented from URL words', () => {
  const t = setup();
  assert.equal(show(t, t.img(0, 0, 'https://a/original-full-large.jpg'))[0], false);
});

test('source change re-evaluates (low-res -> compatible)', () => {
  const t = setup(), im = t.img(...BAD);
  assert.deepEqual(show(t, im), [true, 'btn low']);
  im.srcset = 'https://a/big.jpg 3000w';
  t.mos[0]([{ type: 'attributes', target: im }]);
  t.advance(100);
  assert.equal(t.parts.btn.className, 'btn');
});

test('dimensions arriving on load: hidden -> shown', () => {
  const t = setup(), im = t.img(0, 0);
  assert.equal(show(t, im)[0], false);
  im.naturalWidth = 1920; im.naturalHeight = 1080;
  t.fire('load', { target: im });
  t.advance(10);
  assert.deepEqual([t.visible(), t.parts.btn.className], [true, 'btn']);
});

test('video behaviour unchanged: incompatible video hidden, never low-res', () => {
  const t = setup();
  assert.equal(show(t, t.vid(...BAD))[0], false);
});

test('thresholds unchanged', () => {
  require('../compat.js');
  const { THRESHOLDS } = globalThis.MotivaCompat;
  assert.deepEqual({ ...THRESHOLDS }, { MAX_UPSCALE: 1.25, MAX_CROP: 0.3 });
});

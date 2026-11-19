// Run: node --test scripts/extension/tests
// Drives scripts/extension/background.js with a fake native port and a fake
// clock: checks that the final toast reflects Motiva's real final reply, that
// progress replies do not finish a request, and timeout / disconnect / stale
// reply / duplicate-click behaviour.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const src = fs.readFileSync(path.join(__dirname, '../background.js'), 'utf8');

function setup() {
  let now = 0, seq = 0;
  const timers = new Map();
  const setTimeout_ = (fn, ms) => { const id = ++seq; timers.set(id, { fn, at: now + (ms || 0) }); return id; };
  const clearTimeout_ = (id) => timers.delete(id);
  const advance = async (ms) => {
    const end = now + ms;
    for (;;) {
      let next = null;
      for (const [id, t] of timers) if (t.at <= end && (!next || t.at < next.t.at)) next = { id, t };
      if (!next) break;
      timers.delete(next.id);
      now = next.t.at;
      next.t.fn();
      await flush();
    }
    now = end;
  };
  const flush = async () => { for (let i = 0; i < 20; i++) await Promise.resolve(); };

  const toasts = [];
  const sent = [];
  let onMessage = null, onDisconnect = null;
  const port = {
    postMessage: (m) => sent.push(m),
    onMessage: { addListener: (f) => (onMessage = f) },
    onDisconnect: { addListener: (f) => (onDisconnect = f) },
  };
  const noopEvent = { addListener() {} };
  const chrome = {
    runtime: {
      connectNative: () => port,
      onMessage: { addListener() {} },
      onInstalled: noopEvent,
      onStartup: noopEvent,
      lastError: null,
    },
    tabs: {
      sendMessage: (tabId, msg) => { if (msg.type === 'toast') toasts.push({ text: msg.text, level: msg.level }); return Promise.resolve(); },
      onActivated: noopEvent,
      onUpdated: noopEvent,
    },
    contextMenus: { update: () => Promise.resolve(), removeAll: () => Promise.resolve(), create() {}, onClicked: noopEvent },
    storage: { local: { get: async () => ({}), set: async () => {} } },
  };
  const ctx = {
    chrome, console: { info() {}, warn() {}, log() {} }, setTimeout: setTimeout_, clearTimeout: clearTimeout_,
    performance: { now: () => now }, URL, atob, btoa, crypto: require('node:crypto').webcrypto, Uint8Array,
    fetch: async () => { throw new Error('no network in test'); },
  };
  vm.createContext(ctx);
  // 'let/const' declarations are not properties of the context: expose what the test needs.
  vm.runInContext(src + '\n;this.__sendMedia = sendMedia;', ctx);
  const reply = (obj) => onMessage(obj);
  const lastOf = (type) => [...sent].reverse().find((m) => m.type === type);
  const start = (over = {}) =>
    ctx.__sendMedia({ action: 'wallpaper', src: 'https://example.com/a.jpg', kind: 'image', tabId: 1, frameId: 0, ...over });
  return { toasts, sent, reply, lastOf, start, advance, flush, disconnect: () => onDisconnect(), ctx };
}

// fetch always fails in setup(), so uploadViaBrowser throws and Motiva is asked to download (no upload id).

test('success only after the final ok reply; progress keeps the sending state', async () => {
  const t = setup();
  const p = t.start();
  await t.flush();
  const media = t.lastOf('media');
  assert.ok(media, 'media request sent');
  assert.equal(t.toasts.at(-1).level, 'pending');
  t.reply({ id: media.id, ok: true, progress: 'received' });
  t.reply({ id: media.id, ok: true, progress: 'applying' });
  await t.flush();
  assert.ok(t.toasts.every((x) => x.level === 'pending'), 'no success while only progress was received');
  t.reply({ id: media.id, ok: true, message: 'Motiva: "a.jpg" set as wallpaper' });
  await p;
  assert.deepEqual(t.toasts.at(-1), { text: 'Motiva: "a.jpg" set as wallpaper', level: 'ok' });
});

test('failure reason from Motiva is shown as an error', async () => {
  const t = setup();
  const p = t.start();
  await t.flush();
  const media = t.lastOf('media');
  t.reply({ id: media.id, ok: false, error: 'Unsupported media.' });
  await p;
  assert.deepEqual(t.toasts.at(-1), { text: 'Motiva: Unsupported media.', level: 'error' });
});

test('timeout reports an error, never success, and a late reply is ignored', async () => {
  const t = setup();
  const p = t.start();
  await t.flush();
  const media = t.lastOf('media');
  await t.advance(2 * 60 * 1000 + 10);
  await p;
  const last = t.toasts.at(-1);
  assert.equal(last.level, 'error');
  assert.match(last.text, /did not confirm in time/);
  const count = t.toasts.length;
  t.reply({ id: media.id, ok: true, message: 'Motiva: late' });
  await t.flush();
  assert.equal(t.toasts.length, count, 'stale reply must not change a finished request');
});

test('progress restarts the idle timeout', async () => {
  const t = setup();
  const p = t.start();
  await t.flush();
  const media = t.lastOf('media');
  await t.advance(100 * 1000);
  t.reply({ id: media.id, ok: true, progress: 'applying' });
  await t.advance(100 * 1000); // 200 s total, but only 100 s since the last progress
  assert.ok(t.toasts.every((x) => x.level !== 'error'));
  t.reply({ id: media.id, ok: true, message: 'Motiva: done' });
  await p;
  assert.equal(t.toasts.at(-1).level, 'ok');
});

test('disconnected host gives an error toast', async () => {
  const t = setup();
  const p = t.start();
  await t.flush();
  t.disconnect();
  await p;
  assert.equal(t.toasts.at(-1).level, 'error');
});

test('replies are matched by id (out of order) and duplicates are ignored', async () => {
  const t = setup();
  const p1 = t.start({ src: 'https://example.com/a.jpg' });
  const p2 = t.start({ src: 'https://example.com/b.jpg' });
  await t.flush();
  const medias = t.sent.filter((m) => m.type === 'media');
  assert.equal(medias.length, 2);
  t.reply({ id: medias[1].id, ok: true, message: 'second' });
  t.reply({ id: medias[0].id, ok: false, error: 'first failed' });
  t.reply({ id: medias[1].id, ok: false, error: 'duplicate' });
  await Promise.all([p1, p2]);
  const finals = t.toasts.filter((x) => x.level !== 'pending');
  assert.deepEqual(finals.map((x) => x.text).sort(), ['Motiva: first failed', 'second']);
});

test('a repeated click on the same item does not start a second request', async () => {
  const t = setup();
  const p = t.start();
  await t.flush();
  await t.start(); // same action + source while the first is pending
  assert.equal(t.sent.filter((m) => m.type === 'media').length, 1);
  t.reply({ id: t.lastOf('media').id, ok: true, message: 'ok' });
  await p;
});

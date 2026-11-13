// Run: node --test extension/tests
const test = require('node:test');
const assert = require('node:assert');
require('../compat.js');
const { decide } = globalThis.MotivaCompat;

const D = { width: 1920, height: 1080 };
const GOOD = { width: 1920, height: 1080 };
const BAD = { width: 640, height: 360 };
const vis = (en, m, d = D) => decide(en, m, d).visible;

test('compatible image/video -> visible', () => {
  assert.equal(vis(true, GOOD), true);
  assert.equal(vis(true, { width: 3840, height: 2160 }), true);
  assert.equal(vis(true, { width: 2560, height: 1080 }), true);
});
test('incompatible image/video -> hidden', () => {
  assert.equal(vis(true, BAD), false);
  assert.equal(vis(true, { width: 1080, height: 1920 }), false);
});
test('compatible -> incompatible -> compatible', () => {
  assert.deepEqual([GOOD, BAD, GOOD].map((m) => vis(true, m)), [true, false, true]);
});
test('incompatible -> compatible -> incompatible', () => {
  assert.deepEqual([BAD, GOOD, BAD].map((m) => vis(true, m)), [false, true, false]);
});
test('toggle disabled restores previous behaviour (always visible)', () => {
  assert.equal(vis(false, BAD), true);
  assert.equal(vis(false, { width: 0, height: 0 }), true);
  assert.equal(decide(false, BAD, D).state, 'disabled');
});
test('missing or invalid metadata is explicit unknown, hidden, no throw', () => {
  for (const m of [undefined, null, {}, { width: 0, height: 0 }, { width: NaN, height: 5 }, { width: -1, height: 9 }]) {
    const d = decide(true, m, D);
    assert.equal(d.state, 'unknown');
    assert.equal(d.visible, false);
  }
});
test('unusable display size (screen.* = 0) falls back to previous behaviour, not hide-all', () => {
  for (const d of [undefined, null, {}, { width: 0, height: 0 }, { width: NaN, height: 1080 }]) {
    for (const m of [GOOD, BAD, { width: 0, height: 0 }]) {
      const r = decide(true, m, d);
      assert.equal(r.state, 'display-unknown');
      assert.equal(r.visible, true);
    }
  }
});
test('unknown becomes visible once metadata arrives (no cached unknown)', () => {
  assert.equal(vis(true, { width: 0, height: 0 }), false);
  assert.equal(vis(true, GOOD), true);
});
test('display change re-evaluates', () => {
  assert.equal(vis(true, GOOD, D), true);
  assert.equal(vis(true, GOOD, { width: 3840, height: 2160 }), false);
});

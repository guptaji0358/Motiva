// Motiva browser extension - wallpaper compatibility evaluation.
// Pure logic, no DOM or storage access, shared by images and videos.
// Loaded before content.js (see manifest.json) and exposed as MotivaCompat.
//
// Model: the wallpaper is drawn "cover" style (uniform scale so the media
// fills the display, the overflow is cropped). From the media size and the
// display size we derive:
//   scale     = max(dw/mw, dh/mh)      > 1 means the media is magnified
//   cropFrac  = 1 - min(mAR,dAR)/max(mAR,dAR)   share of the media cut off
// Media is compatible when it is not magnified more than MAX_UPSCALE and
// not cropped more than MAX_CROP. Resolution categories (HD, 4K, ...) are
// never used; only the real pixel sizes and aspect ratios.

(() => {
  const THRESHOLDS = Object.freeze({
    MAX_UPSCALE: 1.25, // up to 25% magnification before softness/zoom is visible
    MAX_CROP: 0.3, // at most 30% of the picture may be cropped away
  });

  function evaluate(media, display, thresholds) {
    const t = Object.assign({}, THRESHOLDS, thresholds || {});
    const mw = media && media.width;
    const mh = media && media.height;
    const dw = display && display.width;
    const dh = display && display.height;
    if (!(mw > 0 && mh > 0 && dw > 0 && dh > 0)) {
      return { known: false, compatible: false, reason: 'unknown-size' };
    }
    const mAR = mw / mh;
    const dAR = dw / dh;
    const scale = Math.max(dw / mw, dh / mh);
    const cropFraction = 1 - Math.min(mAR, dAR) / Math.max(mAR, dAR);
    let reason = 'ok';
    if (scale > t.MAX_UPSCALE) reason = 'too-small';
    else if (cropFraction > t.MAX_CROP) reason = 'aspect-mismatch';
    return { known: true, compatible: reason === 'ok', reason, scale, cropFraction, mAR, dAR };
  }

  // Cache keyed by media size + display size, so it is re-evaluated
  // automatically whenever either changes. Bounded to stay small.
  const cache = new Map();
  function evaluateCached(media, display) {
    const key = `${media.width}x${media.height}@${display.width}x${display.height}`;
    let r = cache.get(key);
    if (!r) {
      r = evaluate(media, display);
      if (cache.size > 256) cache.clear();
      cache.set(key, r);
    }
    return r;
  }

  globalThis.MotivaCompat = { THRESHOLDS, evaluate, evaluateCached };
})();

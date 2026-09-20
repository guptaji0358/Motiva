/* Motiva website — animation behaviour.
   Works with css/animations.css. Everything here is skipped or reduced when the user
   prefers reduced motion, and nothing runs continuously in JavaScript: ambient motion is
   pure CSS (paused when offscreen), and the parallax only updates while the pointer moves. */
(function () {
  'use strict';

  var reduceQuery = window.matchMedia('(prefers-reduced-motion: reduce)');
  var $$ = function (sel, root) { return Array.prototype.slice.call((root || document).querySelectorAll(sel)); };

  /* ------------------------------------------------------------------ hero entrance */
  function heroEntrance() {
    // Let the initial (hidden) styles paint first. rAF does not run in background tabs, so a
    // short timer is the fallback: the hero must never stay invisible.
    var done = false;
    function ready() {
      if (done) { return; }
      done = true;
      document.documentElement.classList.add('is-ready');
    }
    window.requestAnimationFrame(ready);
    window.setTimeout(ready, 80);
  }

  /* ------------------------------------------------------------------ scroll reveal */
  function scrollReveal() {
    var items = $$('.reveal');
    if (!items.length) { return; }

    if (reduceQuery.matches || !('IntersectionObserver' in window)) {
      items.forEach(function (el) { el.classList.add('is-visible'); });
      return;
    }
    var io = new IntersectionObserver(function (entries, obs) {
      entries.forEach(function (entry) {
        if (!entry.isIntersecting) { return; }
        entry.target.classList.add('is-visible');
        obs.unobserve(entry.target);          // reveal once, then stop watching
      });
    }, { threshold: 0.08, rootMargin: '0px 0px -4% 0px' });
    items.forEach(function (el) { io.observe(el); });
  }

  window.addEventListener('beforeprint', function () {
    $$('.reveal').forEach(function (el) { el.classList.add('is-visible'); });
  });

  /* ------------------------------------------------------------------ header state */
  function headerState() {
    var header = document.querySelector('.site-header');
    if (!header) { return; }
    var ticking = false;
    function update() { header.classList.toggle('is-scrolled', window.scrollY > 12); ticking = false; }
    window.addEventListener('scroll', function () {
      if (!ticking) { ticking = true; window.requestAnimationFrame(update); }
    }, { passive: true });
    update();
  }

  /* ------------------------------------------------------------------ pause ambient motion when not visible */
  function pauseWhenHidden() {
    var hero = document.querySelector('.hero');
    if (!hero) { return; }
    var offscreen = false;

    function apply() {
      var paused = offscreen || document.hidden;
      if (paused) { hero.setAttribute('data-paused', ''); } else { hero.removeAttribute('data-paused'); }
    }
    if ('IntersectionObserver' in window) {
      new IntersectionObserver(function (entries) {
        offscreen = !entries[0].isIntersecting;
        apply();
      }).observe(hero);
    }
    document.addEventListener('visibilitychange', apply);
  }

  /* ------------------------------------------------------------------ subtle pointer parallax (hero only) */
  function heroParallax() {
    var visual = document.querySelector('[data-parallax]');
    if (!visual || reduceQuery.matches) { return; }
    if (!window.matchMedia('(hover: hover) and (pointer: fine)').matches) { return; }

    var frame = 0, tx = 0, ty = 0;
    function paint() {
      visual.style.setProperty('--px', tx.toFixed(3));
      visual.style.setProperty('--py', ty.toFixed(3));
      frame = 0;
    }
    visual.addEventListener('pointermove', function (e) {
      var r = visual.getBoundingClientRect();
      tx = ((e.clientX - r.left) / r.width - 0.5) * 2;   // -1 .. 1
      ty = ((e.clientY - r.top) / r.height - 0.5) * 2;
      if (!frame) { frame = window.requestAnimationFrame(paint); }
    }, { passive: true });
    visual.addEventListener('pointerleave', function () {
      tx = 0; ty = 0;
      if (!frame) { frame = window.requestAnimationFrame(paint); }
    });
  }

  /* ------------------------------------------------------------------ init */
  function init() {
    heroEntrance();
    scrollReveal();
    headerState();
    pauseWhenHidden();
    heroParallax();
  }
  if (document.readyState === 'loading') { document.addEventListener('DOMContentLoaded', init); }
  else { init(); }
})();

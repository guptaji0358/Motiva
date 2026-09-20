/* Motiva website — behaviour (navigation, tabs, download link).
   Animation-specific code lives in animations.js. No dependencies. */
(function () {
  'use strict';

  /* ------------------------------------------------------------------ config
     The ONLY place to change distribution links. Binaries are distributed via
     GitHub Releases, never stored in this website.
     releaseUrl: the "Download for Windows" target. Direct link to the installer asset on the repository's
     latest release (tag v1.0.0 published). Keep the asset name in sync with the
     file attached to the release. */
  var GITHUB_URL = 'https://github.com/guptaji0358/Motiva';
  var CONFIG = {
    githubUrl: GITHUB_URL,
    releaseUrl: GITHUB_URL + '/releases/latest/download/MotivaSetup.exe'
  };

  var $ = function (sel, root) { return (root || document).querySelector(sel); };
  var $$ = function (sel, root) { return Array.prototype.slice.call((root || document).querySelectorAll(sel)); };

  /* ------------------------------------------------------------------ download / GitHub links */
  function setupLinks() {
    $$('[data-github]').forEach(function (a) { a.href = CONFIG.githubUrl; });

    $$('[data-download], [data-download-main]').forEach(function (a) { a.href = CONFIG.releaseUrl; });
  }

  /* ------------------------------------------------------------------ mobile navigation */
  function setupNav() {
    var toggle = $('.nav-toggle');
    var nav = $('#site-nav');
    var header = $('.site-header');
    if (!toggle || !nav) { return; }

    function setOpen(open) {
      toggle.setAttribute('aria-expanded', String(open));
      toggle.setAttribute('aria-label', open ? 'Close menu' : 'Open menu');
      nav.classList.toggle('is-open', open);
    }

    toggle.addEventListener('click', function () {
      setOpen(toggle.getAttribute('aria-expanded') !== 'true');
    });

    // Close after choosing a destination, on Escape, or when leaving the mobile layout.
    nav.addEventListener('click', function (e) {
      if (e.target.closest('a')) { setOpen(false); }
    });
    document.addEventListener('keydown', function (e) {
      if (e.key === 'Escape' && nav.classList.contains('is-open')) { setOpen(false); toggle.focus(); }
    });
    document.addEventListener('click', function (e) {
      if (nav.classList.contains('is-open') && !header.contains(e.target)) { setOpen(false); }
    });
    window.matchMedia('(min-width: 861px)').addEventListener('change', function (mq) {
      if (mq.matches) { setOpen(false); }
    });
  }

  /* ------------------------------------------------------------------ smooth scrolling
     CSS `scroll-behavior` + `scroll-padding-top` do the work (and honour reduced motion).
     This only moves keyboard focus to the target so screen-reader / keyboard users land there. */
  function setupAnchorFocus() {
    document.addEventListener('click', function (e) {
      var a = e.target.closest('a[href^="#"]');
      if (!a) { return; }
      var id = a.getAttribute('href');
      if (id.length < 2) { return; }
      var target = document.getElementById(id.slice(1));
      if (!target) { return; }
      if (!target.hasAttribute('tabindex')) { target.setAttribute('tabindex', '-1'); }
      // preventScroll: the browser's own smooth scroll is already running
      window.setTimeout(function () { target.focus({ preventScroll: true }); }, 0);
    });
  }

  /* ------------------------------------------------------------------ active nav link */
  function setupActiveNav() {
    var links = $$('.site-nav ul a[href^="#"]');
    var map = {};
    links.forEach(function (a) { map[a.getAttribute('href').slice(1)] = a; });
    var sections = Object.keys(map).map(function (id) { return document.getElementById(id); }).filter(Boolean);
    if (!sections.length || !('IntersectionObserver' in window)) { return; }

    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (!entry.isIntersecting) { return; }
        links.forEach(function (a) { a.removeAttribute('aria-current'); });
        map[entry.target.id].setAttribute('aria-current', 'true');
      });
    }, { rootMargin: '-45% 0px -50% 0px' });
    sections.forEach(function (s) { io.observe(s); });
  }

  /* ------------------------------------------------------------------ screenshot tabs (WAI-ARIA tabs pattern) */
  function setupTabs() {
    var root = $('[data-tabs]');
    if (!root) { return; }
    var tabs = $$('[role="tab"]', root);
    var panels = $$('[role="tabpanel"]', root);

    function select(tab, focus) {
      tabs.forEach(function (t) {
        var on = t === tab;
        t.setAttribute('aria-selected', String(on));
        t.tabIndex = on ? 0 : -1;
      });
      panels.forEach(function (p) { p.hidden = p.id !== tab.getAttribute('aria-controls'); });
      if (focus) { tab.focus(); }
    }

    tabs.forEach(function (tab, i) {
      tab.addEventListener('click', function () { select(tab, false); });
      tab.addEventListener('keydown', function (e) {
        var next = null;
        if (e.key === 'ArrowDown' || e.key === 'ArrowRight') { next = tabs[(i + 1) % tabs.length]; }
        else if (e.key === 'ArrowUp' || e.key === 'ArrowLeft') { next = tabs[(i - 1 + tabs.length) % tabs.length]; }
        else if (e.key === 'Home') { next = tabs[0]; }
        else if (e.key === 'End') { next = tabs[tabs.length - 1]; }
        if (next) { e.preventDefault(); select(next, true); }
      });
    });
  }

  /* ------------------------------------------------------------------ init */
  function init() {
    setupLinks();
    setupNav();
    setupAnchorFocus();
    setupActiveNav();
    setupTabs();
  }

  if (document.readyState === 'loading') { document.addEventListener('DOMContentLoaded', init); }
  else { init(); }
})();

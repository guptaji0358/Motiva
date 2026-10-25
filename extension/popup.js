const dot = document.getElementById('dot');
const state = document.getElementById('state');
const detail = document.getElementById('detail');
const overlay = document.getElementById('overlay');

chrome.storage.local.get('overlay').then((d) => {
  overlay.checked = d.overlay !== false;
});
overlay.addEventListener('change', () => chrome.storage.local.set({ overlay: overlay.checked }));

chrome.runtime.sendMessage({ type: 'status' }).then((r) => {
  if (r && r.connected) {
    dot.className = 'dot ok';
    state.textContent = 'Connected to Motiva';
    detail.textContent = r.appRunning ? 'Motiva is running.' : 'Motiva starts when you send something.';
  } else {
    dot.className = 'dot bad';
    state.textContent = 'Motiva helper not found';
    detail.textContent = (r && r.error) || 'Turn on "Browser extension" in Motiva > Settings > Integration.';
  }
});

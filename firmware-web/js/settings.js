// settings.js - minimal integration with firmware /api/v1/settings
(async function () {
  function getToken() {
    return localStorage.getItem('neo_token') || '';
  }

  async function getSettings() {
    const token = getToken();
    if (!token) return;
    try {
      const res = await fetch('/api/v1/settings', { headers: { 'Authorization': 'Bearer ' + token } });
      if (!res.ok) return;
      const j = await res.json();
      document.getElementById('settings-device-name').value = j.device_name || '';
      const sel = document.getElementById('settings-keyboard-layout');
      if (sel && j.keyboard_layout) sel.value = j.keyboard_layout;
      // other fields
    } catch (e) {
      console.error('getSettings error', e);
    }
  }

  async function saveSettings() {
    const token = getToken();
    if (!token) {
      showStatus('Not signed in');
      return;
    }
    const payload = {
      device_name: document.getElementById('settings-device-name').value,
      keyboard_layout: document.getElementById('settings-keyboard-layout').value,
    };
    try {
      const res = await fetch('/api/v1/settings', {
        method: 'POST',
        headers: { 'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
      });
      if (res.ok) {
        showStatus('Saved', true);
      } else {
        showStatus('Save failed');
      }
    } catch (e) {
      showStatus('Save error');
    }
  }

  function showStatus(msg, ok) {
    const el = document.getElementById('settings-note') || document.getElementById('settings-status');
    if (!el) return;
    el.textContent = msg;
    if (ok) el.style.color = 'green'; else el.style.color = '';
    setTimeout(() => { el.textContent = ''; }, 3000);
  }

  document.addEventListener('DOMContentLoaded', () => {
    // populate settings if token exists
    getSettings();
    const form = document.getElementById('settings-form');
    if (form) {
      form.addEventListener('submit', (ev) => {
        ev.preventDefault();
        saveSettings();
      });
    }
  });
})();

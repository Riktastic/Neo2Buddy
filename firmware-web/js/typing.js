/**
 * Live typing + Bluetooth page — depends on js/core.js (+ optional settings.js).
 *
 * Polling policy:
 * - /status only on page load, login, and when the buddy becomes unreachable
 * - /keyboard/recent uses ?since=&kb=&usb= → 304 when unchanged
 * - 1s while text/USB state is changing; 2.5s when idle
 */
(function () {
  'use strict';
  if (document.body?.dataset?.page !== 'typing' || !window.Neo2) return;

  const $ = (sel) => document.querySelector(sel);
  const authFetch = (path, opts) => Neo2.authFetch(path, opts);
  const escapeHtml = (v) => Neo2.escapeHtml(v);
  const showNotice = (msg, type) => Neo2.showNotice(msg, type);

  const POLL_ACTIVE_MS = 1000;
  const POLL_IDLE_MS = 2500;
  const IDLE_AFTER_UNCHANGED = 2;

  const liveText = document.getElementById('live-text');
  const dialog = document.getElementById('action-dialog');
  let liveSequence = -1;
  let liveTextContent = '';
  let livePaused = false;
  let liveRaw = false;
  let liveFollow = true;
  let sNeoKeyboardActive = false;
  let sUsbConnected = false;
  let sBleConnected = false;
  let blePreviewLength = 0;
  let pollTimer = null;
  let unchangedStreak = 0;
  let statusOnDisconnectArmed = true;

  function setFollowUI(enabled) {
    liveFollow = !!enabled;
    const followEl = document.getElementById('follow-indicator');
    const pauseBtn = document.getElementById('pause-live');
    if (followEl) {
      followEl.textContent = liveFollow ? (livePaused ? 'Paused' : 'Follow') : 'Hold';
    }
    if (pauseBtn) {
      pauseBtn.textContent = livePaused ? 'Resume' : liveFollow ? 'Pause' : 'Paused';
    }
  }

  function renderLiveTextBody(text) {
    const lines = (text || '').split(/\r?\n/);
    let html = '';
    for (let i = 0; i < lines.length; ++i) {
      const cls = i === lines.length - 1 ? 'live-line new' : 'live-line';
      const content = lines[i] === '' ? '\u00A0' : escapeHtml(lines[i]);
      html += `<div class="${cls}">${content}</div>`;
    }
    if (!html) html = '<div class="live-line">Waiting for NEO keyboard input...</div>';
    if (liveText) {
      liveText.innerHTML = html;
      if (liveFollow) liveText.scrollTop = liveText.scrollHeight;
    }
  }

  function stopLivePoll() {
    if (pollTimer) {
      clearTimeout(pollTimer);
      pollTimer = null;
    }
  }

  function updatePortalNotices() {
    const el = document.getElementById('portal-notices');
    if (!el) return;
    if (!Neo2.getAuthToken()) {
      el.innerHTML = '';
      el.hidden = true;
      return;
    }
    let html = '';
    if (sNeoKeyboardActive) {
      html =
        '<p class="portal-notice portal-notice-info" role="status">Keyboard mode — live feed uses cheap 304 polls when idle; device status only on load or disconnect.</p>';
    }
    el.innerHTML = html;
    el.hidden = !html;
  }

  function setNeoConnectionState(status) {
    if (!Neo2.getAuthToken()) return;
    const connected = !!status?.usb_connected;
    const keyboardActive = !!status?.usb_keyboard_active;
    const commsReady = !!status?.usb_neo_ready;
    sUsbConnected = connected;
    sNeoKeyboardActive = !!(keyboardActive && !commsReady);
    const product = status?.product || '';
    const bakBusy = !!status?.auto_backup_busy;
    const conn = $('#neo-connection');
    const detail = $('#neo-connection-detail');
    const model = $('#neo-model');
    const modelDetail = $('#neo-model-detail');
    if (conn) {
      conn.textContent = bakBusy
        ? 'Backing up…'
        : connected
          ? keyboardActive && !commsReady
            ? 'Keyboard'
            : 'Ready'
          : 'Waiting';
    }
    if (detail) {
      if (bakBusy) detail.textContent = 'Auto-backup in progress…';
      else if (connected) {
        detail.textContent =
          keyboardActive && !commsReady
            ? 'Keyboard mode — live typing active'
            : 'ASM (manager) mode — use Keyboard mode below';
      } else detail.textContent = 'Connect the NEO with USB';
    }
    if (model) model.textContent = connected || bakBusy ? product || 'NEO' : '--';
    if (modelDetail) {
      modelDetail.textContent = !connected && !bakBusy
        ? 'Reported by the NEO when connected'
        : sNeoKeyboardActive
          ? `${product || 'Neo2'} · keyboard mode`
          : product || 'Neo connected';
    }
    const restartBtn = $('#neo-restart');
    if (restartBtn && Neo2.getAuthToken()) {
      /* Visible whenever Neo is plugged in so user can leave ASM after Documents actions. */
      restartBtn.hidden = !connected && !bakBusy;
      restartBtn.disabled = !!bakBusy;
    }
    updatePortalNotices();
  }

  function applyKeyboardFeedStatus(payload) {
    if (!payload || typeof payload !== 'object') return;
    setNeoConnectionState({
      usb_connected: !!payload.usb_connected,
      usb_keyboard_active: !!payload.usb_keyboard_active,
      usb_neo_ready: !!payload.usb_neo_ready,
      product: $('#neo-model')?.textContent !== '--' ? $('#neo-model').textContent : '',
    });
  }

  async function noteBuddyUnreachable() {
    if (!Neo2.getAuthToken() || !statusOnDisconnectArmed) return;
    statusOnDisconnectArmed = false;
    try {
      await Neo2.refreshStatus();
    } catch (_) {}
  }

  function scheduleLivePoll(delayMs) {
    if (!Neo2.getAuthToken()) {
      stopLivePoll();
      return;
    }
    if (pollTimer) clearTimeout(pollTimer);
    pollTimer = setTimeout(() => {
      pollTimer = null;
      if (!Neo2.getAuthToken()) return;
      refreshLiveText().finally(() => {
        if (!Neo2.getAuthToken()) return;
        const next =
          livePaused || document.hidden
            ? POLL_IDLE_MS
            : unchangedStreak >= IDLE_AFTER_UNCHANGED
              ? POLL_IDLE_MS
              : POLL_ACTIVE_MS;
        scheduleLivePoll(next);
      });
    }, delayMs);
  }

  async function refreshLiveText() {
    if (!Neo2.getAuthToken() || livePaused || !liveText) return;
    try {
      if (liveRaw) {
        const res = await authFetch('/keyboard/raw?limit=32', { cache: 'no-store' });
        if (!res.ok) {
          if (res.status >= 500 || res.status === 0) await noteBuddyUnreachable();
          liveText.innerHTML = `<div class="live-line live-error">Raw HID view unavailable (${res.status}).</div>`;
          unchangedStreak++;
          return;
        }
        statusOnDisconnectArmed = true;
        const arr = await res.json().catch(() => []);
        if (!Array.isArray(arr)) return;
        const html =
          arr
            .map((e) => {
              const t = new Date((e.ts || 0) * 1000).toLocaleTimeString();
              return `<div class="live-line"><div class="live-meta"><span class="live-ts">${t}</span></div><div class="live-hex">${escapeHtml(e.data_hex || '')}</div></div>`;
            })
            .join('') || '<div class="live-line">No recent raw HID reports</div>';
        liveText.innerHTML = html;
        if (liveFollow) liveText.scrollTop = liveText.scrollHeight;
        unchangedStreak = arr.length ? 0 : unchangedStreak + 1;
        return;
      }

      const kb = sNeoKeyboardActive ? 1 : 0;
      const usb = sUsbConnected ? 1 : 0;
      let url = '/keyboard/recent';
      const headers = {};
      if (liveSequence >= 0) {
        url += `?since=${liveSequence}&kb=${kb}&usb=${usb}`;
        headers['If-None-Match'] = `"${liveSequence}:${kb}:${usb}"`;
      }

      const response = await authFetch(url, { cache: 'no-store', headers });
      if (response.status === 304) {
        statusOnDisconnectArmed = true;
        unchangedStreak++;
        if (!sNeoKeyboardActive && !liveRaw) {
          liveText.innerHTML =
            '<div class="live-line">Neo is in manager mode — tap <strong>Keyboard mode</strong> above to resume live typing.</div>';
        }
        return;
      }
      if (!response.ok) {
        if (response.status >= 500 || response.status === 408 || response.status === 0) {
          await noteBuddyUnreachable();
        }
        liveText.innerHTML = `<div class="live-line live-error">Live keyboard feed unavailable (${response.status}).</div>`;
        unchangedStreak++;
        return;
      }
      statusOnDisconnectArmed = true;
      const payload = await response.json().catch(() => null);
      if (!payload || typeof payload.text !== 'string') {
        liveText.innerHTML = '<div class="live-line live-error">Unexpected live keyboard response.</div>';
        unchangedStreak++;
        return;
      }

      const prevSeq = liveSequence;
      const prevKb = sNeoKeyboardActive;
      const prevUsb = sUsbConnected;
      applyKeyboardFeedStatus(payload);

      const sequence = Number(payload.sequence);
      const changed =
        (Number.isFinite(sequence) && sequence !== prevSeq) ||
        payload.text !== liveTextContent ||
        sNeoKeyboardActive !== prevKb ||
        sUsbConnected !== prevUsb;

      if (Number.isFinite(sequence)) liveSequence = sequence;
      liveTextContent = payload.text;

      if (!sNeoKeyboardActive) {
        liveText.innerHTML =
          '<div class="live-line">Neo is in manager mode — tap <strong>Keyboard mode</strong> above to resume live typing.</div>';
      } else {
        renderLiveTextBody(payload.text);
      }
      unchangedStreak = changed ? 0 : unchangedStreak + 1;
    } catch (error) {
      await noteBuddyUnreachable();
      liveText.innerHTML = `<div class="live-line live-error">${escapeHtml(error.message || 'Could not reach the live keyboard feed.')}</div>`;
      unchangedStreak++;
    }
  }

  let blePairingPoll = null;
  function stopBlePairingPoll() {
    if (blePairingPoll) {
      clearInterval(blePairingPoll);
      blePairingPoll = null;
    }
  }

  function bleStatusLabel(j) {
    if (j.state === 'connected') return 'Connected — Neo keys pass through live';
    if (j.state === 'pairing' || j.pairing_enabled) {
      if (j.advertising) {
        return 'Advertising now — choose Neo2 Buddy and tap Pair (no PIN, 3 min)';
      }
      if (j.ready) return 'Bluetooth starting — advertising should begin in a moment…';
      return 'Pairing requested, but Bluetooth is not advertising yet';
    }
    if (j.bonded > 0) {
      return `Idle — ${j.bonded} bonded host${j.bonded === 1 ? '' : 's'}`;
    }
    return 'Bluetooth off — start pairing to add a host';
  }

  async function refreshBleStatus() {
    const line = $('#ble-status-line');
    const bondsList = $('#ble-bonds-list');
    if (!line) return;
    try {
      const res = await authFetch('/ble');
      if (!res.ok) throw new Error('unavailable');
      const j = await res.json();
      const sendLine = $('#ble-send-status');
      line.textContent = `${bleStatusLabel(j)}. ${j.can_send ? 'Host ready.' : 'Connect or pair a Bluetooth host.'}`;
      if (bondsList) {
        const bonds = Array.isArray(j.bonds) ? j.bonds : [];
        if (bonds.length === 0) {
          bondsList.hidden = true;
          bondsList.innerHTML = '';
        } else {
          bondsList.hidden = false;
          bondsList.innerHTML = bonds.map((b, i) => {
            const addr = (b && b.addr) ? String(b.addr) : 'unknown';
            return `<li>Host ${i + 1}: <code>${addr}</code></li>`;
          }).join('');
        }
      }
      if (j.state === 'pairing' || j.pairing_enabled) {
        if (!blePairingPoll) blePairingPoll = setInterval(() => { refreshBleStatus(); }, 1500);
      } else {
        stopBlePairingPoll();
      }
      if (sendLine) {
        sendLine.textContent = j.can_send
          ? 'Host ready. Neo keys pass through; portal text send is optional.'
          : 'Waiting for Bluetooth host connection...';
      }
    } catch (_) {
      line.textContent = 'Bluetooth status unavailable.';
      if (bondsList) {
        bondsList.hidden = true;
        bondsList.innerHTML = '';
      }
    }
  }

  const actions = {
    'open-pairing': [
      'BLUETOOTH',
      'Pair keyboard',
      'Make the buddy discoverable so a phone or computer can bond as a Bluetooth keyboard. No PIN — tap Pair/Connect on the host. Turn off Bluetooth on nearby PCs while pairing. Bluetooth stays off until you start pairing or a host is already saved.',
    ],
    'open-ble-send': [
      'BLUETOOTH',
      'Send text',
      'Preview portal text and type it to the paired host.',
    ],
  };

  document.querySelectorAll('[data-action]').forEach((button) => {
    button.addEventListener('click', () => {
      const action = button.dataset.action;
      if (action === 'settings' || action === 'wifi') {
        if (typeof Neo2.openSettings === 'function') {
          Neo2.openSettings({ focusNetwork: action === 'wifi' });
        } else {
          document.getElementById('settings-dialog')?.showModal();
        }
        return;
      }
      if (!Neo2.getAuthToken()) {
        showNotice('Sign in to use Bluetooth actions.', 'error');
        return;
      }
      const meta = actions[action];
      if (!meta || !dialog) return;
      $('#dialog-label').textContent = meta[0];
      $('#dialog-title').textContent = meta[1];
      $('#dialog-copy').textContent = meta[2];
      $('#dialog-actions-default').hidden = true;
      $('#dialog-actions-pairing').hidden = action !== 'open-pairing';
      $('#dialog-actions-ble-send').hidden = action !== 'open-ble-send';
      if (action === 'open-pairing') refreshBleStatus();
      if (action === 'open-ble-send') {
        $('#ble-send-text').value = '';
        $('#ble-preview-box').hidden = true;
        $('#ble-send-status').textContent = '';
        blePreviewLength = 0;
        refreshBleStatus();
      }
      dialog.showModal();
    });
  });

  $('#action-dialog-close')?.addEventListener('click', () => dialog?.close());

  $('#ble-start-pairing')?.addEventListener('click', async () => {
    const line = $('#ble-status-line');
    if (line) line.textContent = 'Starting Bluetooth…';
    try {
      const res = await authFetch('/ble/pairing', {
        method: 'POST',
        body: JSON.stringify({ enabled: true }),
        headers: { 'Content-Type': 'application/json' },
      });
      const j = await res.json().catch(() => ({}));
      if (!res.ok) throw new Error(j.error || 'Pairing could not be started.');
      showNotice(
        j.advertising
          ? 'Device is advertising — choose Neo2 Buddy and tap Pair (no PIN).'
          : 'Bluetooth started — waiting for advertising…',
        'success'
      );
      refreshBleStatus();
    } catch (error) {
      stopBlePairingPoll();
      if (line) line.textContent = error.message || 'Pairing could not be started.';
      showNotice(error.message, 'error');
    }
  });
  $('#ble-stop-pairing')?.addEventListener('click', async () => {
    try {
      await authFetch('/ble/pairing', {
        method: 'POST',
        body: JSON.stringify({ enabled: false }),
        headers: { 'Content-Type': 'application/json' },
      });
      stopBlePairingPoll();
      showNotice('Pairing stopped.', 'success');
      refreshBleStatus();
    } catch (error) {
      showNotice(error.message, 'error');
    }
  });
  $('#ble-clear-bonds')?.addEventListener('click', async () => {
    const line = $('#ble-status-line');
    if (!window.confirm('Forget all bonded Bluetooth hosts on this buddy? You will need to pair again.')) {
      return;
    }
    if (line) line.textContent = 'Forgetting bonded hosts…';
    try {
      const res = await authFetch('/ble/bonds/clear', {
        method: 'POST',
        body: '{}',
        headers: { 'Content-Type': 'application/json' },
      });
      const j = await res.json().catch(() => ({}));
      if (!res.ok) {
        throw new Error(j.error || 'Could not clear bonded hosts.');
      }
      stopBlePairingPoll();
      showNotice('Bonded hosts forgotten. Start pairing to add a new one.', 'success');
      refreshBleStatus();
    } catch (error) {
      if (line) line.textContent = error.message || 'Could not clear bonded hosts.';
      showNotice(error.message, 'error');
    }
  });
  $('#ble-preview-btn')?.addEventListener('click', async () => {
    const text = $('#ble-send-text')?.value || '';
    const status = $('#ble-send-status');
    const previewBox = $('#ble-preview-box');
    if (!text.trim()) {
      if (status) status.textContent = 'Enter text to preview.';
      return;
    }
    try {
      const res = await authFetch('/ble/preview', {
        method: 'POST',
        body: JSON.stringify({ text }),
        headers: { 'Content-Type': 'application/json' },
      });
      if (!res.ok) throw new Error('Preview failed.');
      const j = await res.json();
      blePreviewLength = j.length || text.length;
      if (previewBox) {
        previewBox.hidden = false;
        previewBox.textContent = j.preview || text.slice(0, 200);
      }
      if (status) {
        status.textContent = `Ready to send ${blePreviewLength} characters. ${j.can_send ? '' : 'Connect a Bluetooth host first.'}`;
      }
    } catch (error) {
      if (status) status.textContent = error.message;
    }
  });
  $('#ble-confirm-send')?.addEventListener('click', async () => {
    const status = $('#ble-send-status');
    if (!blePreviewLength) {
      if (status) status.textContent = 'Preview the text before sending.';
      return;
    }
    if (!confirm(`Send ${blePreviewLength} characters as keystrokes?`)) return;
    try {
      const res = await authFetch('/ble/send', {
        method: 'POST',
        body: '{}',
        headers: { 'Content-Type': 'application/json' },
      });
      if (res.status === 412) throw new Error('No Bluetooth host connected or preview expired.');
      if (!res.ok) throw new Error('Send failed.');
      if (status) status.textContent = 'Sending keystrokes...';
      showNotice('Bluetooth transfer started.', 'success');
    } catch (error) {
      if (status) status.textContent = error.message;
    }
  });
  $('#ble-cancel-send')?.addEventListener('click', async () => {
    try {
      await authFetch('/ble/cancel', { method: 'POST', body: '{}', headers: { 'Content-Type': 'application/json' } });
      blePreviewLength = 0;
      const box = $('#ble-preview-box');
      if (box) box.hidden = true;
      const status = $('#ble-send-status');
      if (status) status.textContent = 'Transfer cancelled.';
    } catch (error) {
      const status = $('#ble-send-status');
      if (status) status.textContent = error.message;
    }
  });

  $('#neo-restart')?.addEventListener('click', async () => {
    const btn = $('#neo-restart');
    if (!window.confirm('Return the NEO to keyboard mode? This leaves ASM/manager mode so live typing and Bluetooth passthrough can resume.')) {
      return;
    }
    if (btn) {
      btn.disabled = true;
      btn.textContent = 'Restarting…';
    }
    try {
      const res = await authFetch('/neo/restart', { method: 'POST', body: '{}', headers: { 'Content-Type': 'application/json' } });
      const j = await Neo2.readJson(res).catch(() => ({}));
      if (!res.ok) throw new Error(j.error || j.message || 'Restart failed');
      showNotice('NEO returned to keyboard mode.', 'success');
      await Neo2.refreshStatus().catch(() => {});
      unchangedStreak = 0;
      refreshLiveText();
      scheduleLivePoll(POLL_ACTIVE_MS);
    } catch (error) {
      showNotice(error.message || 'Could not return Neo to keyboard mode.', 'error');
    } finally {
      if (btn) {
        btn.disabled = false;
        btn.textContent = 'Keyboard mode';
      }
    }
  });

  $('#clear-live')?.addEventListener('click', async () => {
    try {
      await authFetch('/keyboard/clear', { method: 'POST' });
      liveSequence = -1;
      liveTextContent = '';
      unchangedStreak = 0;
      if (liveText) liveText.innerHTML = '<div class="live-line">Monitor cleared.</div>';
      refreshLiveText();
    } catch (_) {}
  });
  $('#pause-live')?.addEventListener('click', () => {
    livePaused = !livePaused;
    if (livePaused) setFollowUI(false);
    else {
      setFollowUI(true);
      unchangedStreak = 0;
      refreshLiveText();
      scheduleLivePoll(POLL_ACTIVE_MS);
    }
  });
  $('#follow-indicator')?.addEventListener('click', () => setFollowUI(!liveFollow));
  $('#toggle-raw')?.addEventListener('click', (ev) => {
    liveRaw = !liveRaw;
    ev.currentTarget.textContent = liveRaw ? 'Decoded' : 'Raw';
    liveSequence = -1;
    liveTextContent = '';
    unchangedStreak = 0;
    refreshLiveText();
    scheduleLivePoll(POLL_ACTIVE_MS);
  });

  Neo2.onStatus = (j) => {
    if (!Neo2.getAuthToken()) return;
    sBleConnected = j.ble_state === 'connected';
    setNeoConnectionState(j);
  };

  Neo2.onAuthChange = (authed) => {
    if (authed) {
      unchangedStreak = 0;
      liveSequence = -1;
      liveTextContent = '';
      Neo2.refreshStatus()
        .then(() => refreshLiveText())
        .catch(() => {})
        .finally(() => scheduleLivePoll(POLL_ACTIVE_MS));
    } else {
      stopLivePoll();
      liveSequence = -1;
      liveTextContent = '';
      sNeoKeyboardActive = false;
      sUsbConnected = false;
      sBleConnected = false;
      unchangedStreak = 0;
      const restartBtn = $('#neo-restart');
      if (restartBtn) {
        restartBtn.hidden = true;
        restartBtn.disabled = false;
        restartBtn.textContent = 'Keyboard mode';
      }
    }
  };

  setFollowUI(true);
  if (Neo2.getAuthToken()) {
    Neo2.refreshStatus()
      .catch(() => {})
      .finally(() => {
        refreshLiveText().finally(() => scheduleLivePoll(POLL_ACTIVE_MS));
      });
  }

  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && Neo2.getAuthToken()) {
      unchangedStreak = 0;
      refreshLiveText();
      scheduleLivePoll(POLL_ACTIVE_MS);
    }
  });
})();

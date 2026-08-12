const PORTAL_PAGE = document.body?.dataset?.page || 'dashboard';
const IS_TYPING_PAGE = PORTAL_PAGE === 'typing';
const IS_DASHBOARD = PORTAL_PAGE === 'dashboard';
const IS_NEO_LINK_PAGE = PORTAL_PAGE === 'neo-link';

const STATUS_ACTIVE_MS = 12000;
const STATUS_IDLE_MS = 30000;
let statusPollTimer = null;
let statusUnchangedStreak = 0;
let statusFingerprint = '';

const dialog = document.querySelector('#action-dialog');
const filesEmpty = document.querySelector('#files-empty');
const fileList = document.querySelector('#file-list');
const appletsEmpty = document.querySelector('#applets-empty');
const appletList = document.querySelector('#applet-list');
const liveText = document.querySelector('#live-text');
const neoFilesEmpty = document.querySelector('#neo-files-empty');
const neoFileTableWrap = document.querySelector('#neo-file-table-wrap');
const neoFileList = document.querySelector('#neo-file-list');
const appletDialog = document.querySelector('#applet-dialog');
const appletForm = document.querySelector('#applet-form');
const documentDialog = document.querySelector('#document-dialog');
const writeDocumentDialog = document.querySelector('#write-document-dialog');
const writeDocumentForm = document.querySelector('#write-document-form');
let appletDialogOperation = null;
let activeDocument = null;
let activeWriteTarget = null;
const setupDialog = document.querySelector('#setup-dialog');
const syncDialog = document.querySelector('#sync-dialog');
const settingsDialog = document.querySelector('#settings-dialog');

const actions = {
  'open-pairing': ['BLUETOOTH', 'Pair keyboard', 'Make the buddy discoverable so a phone or computer can bond as a Bluetooth keyboard. No PIN — tap Pair/Connect on the host. After a firmware update, remove Neo2 Buddy from the host Bluetooth list and Forget bonded hosts here first. Pairing for a new host stops after three minutes. While connected, Neo keys are forwarded live over Bluetooth.'],
  'open-ble-send': ['BLUETOOTH', 'Send text', 'Optionally preview portal text and type it to the paired host. Neo keys already pass through live when Bluetooth is connected — this is for pasting backups or longer text.'],
  wifi: ['NETWORK', 'Wi-Fi setup', 'Switch between Direct access and Home network, or update saved Wi‑Fi details.'],
  settings: ['SETTINGS', 'Device settings', 'Control brightness, sleep, device name and recovery options.']
};

let activeBackupFile = null;
let blePreviewLength = 0;

const API_BASE = '/api/v1';
// The NEO protocol reports the authoritative limit per file. Until that route
// is available, this warning keeps local backups below a conservative 64 KiB.
const LOCAL_BACKUP_WARNING_BYTES = 64 * 1024;
const NEO_ALPHAWORD_ID = 0xA000;

function getNeoCharmap() {
  return document.querySelector('#neo-charmap')?.value || 'en-us';
}

function neoCharmapQuery(prefix = '?') {
  return `${prefix}map=${encodeURIComponent(getNeoCharmap())}`;
}

function getAuthToken() { return localStorage.getItem('neo2_token'); }

/** True only after the device accepts the stored bearer token (or a fresh login). */
let sessionVerified = false;

function isSessionAuthed() {
  return sessionVerified && !!getAuthToken();
}

function setAuthToken(t) {
  if (t) localStorage.setItem('neo2_token', t);
  else {
    localStorage.removeItem('neo2_token');
    localStorage.removeItem('neo2_token_exp_at');
    sessionVerified = false;
  }
}

/** Drop a dead/expired portal session and return the UI to guest mode. */
function invalidateSession(notice) {
  const had = !!getAuthToken() || sessionVerified;
  sessionVerified = false;
  setAuthToken(null);
  if (had) {
    updateSignInState();
    if (notice) showNotice(notice, 'error');
  }
}

function rememberTokenExpiry(expiresInSec) {
  const sec = Number(expiresInSec);
  if (!Number.isFinite(sec) || sec <= 0) {
    localStorage.removeItem('neo2_token_exp_at');
    return;
  }
  localStorage.setItem('neo2_token_exp_at', String(Date.now() + sec * 1000));
}

async function authFetch(path, opts = {}) {
  opts.headers = opts.headers || {};
  const t = getAuthToken();
  if (!t) return Promise.reject(new Error('Authentication required'));
  opts.headers.Authorization = 'Bearer ' + t;
  const res = await fetch(API_BASE + path, opts);
  if (res.status === 401) {
    invalidateSession('Session expired — sign in again');
  }
  return res;
}

/**
 * Confirm the stored token is still accepted (and rotate it).
 * Returns false when the session is gone; network errors return null (keep token, stay guest UI).
 */
async function validateSession() {
  const t = getAuthToken();
  if (!t) {
    sessionVerified = false;
    return false;
  }
  try {
    const res = await fetch(API_BASE + '/token/refresh', {
      method: 'POST',
      headers: { Authorization: 'Bearer ' + t },
    });
    if (res.status === 401) {
      invalidateSession('Session expired — sign in again');
      return false;
    }
    if (!res.ok) {
      sessionVerified = false;
      return null;
    }
    const j = await res.json();
    if (j && j.token) {
      setAuthToken(j.token);
      rememberTokenExpiry(j.expires_in);
    }
    sessionVerified = true;
    return true;
  } catch (_) {
    sessionVerified = false;
    return null;
  }
}

/** Drop a browser token if the buddy is back on first-run setup (NVS wiped). */
async function clearSessionIfOnboarding() {
  try {
    const res = await fetch(`${API_BASE}/onboarding?_=${Date.now()}`, { cache: 'no-store' });
    if (!res.ok) return;
    const j = await res.json().catch(() => null);
    if (j && j.onboarding_complete === false && getAuthToken()) {
      invalidateSession();
    }
  } catch (_) { /* ignore */ }
}

function purgeExpiredLocalToken() {
  const expAt = Number(localStorage.getItem('neo2_token_exp_at') || 0);
  if (getAuthToken() && expAt > 0 && Date.now() > expAt) {
    setAuthToken(null);
  }
}

/** Read-only local endpoints that do not require sign-in on the device AP. */
function localFetch(path, opts={}) {
  opts.cache = 'no-store';
  return fetch(`${API_BASE}${path}${path.includes('?') ? '&' : '?'}_=${Date.now()}`, opts);
}

// --- Logs modal support ---
const logsDialog = document.getElementById('logs-dialog');
const logsList = document.getElementById('logs-list');
const logsFilterGroup = document.getElementById('logs-filter-group');
const logsLimitSelect = document.getElementById('logs-limit-select');
const logsSortSelect = document.getElementById('logs-sort-select');
const logsPrev = document.getElementById('logs-prev');
const logsNext = document.getElementById('logs-next');
const logsSummary = document.getElementById('logs-summary');
const logsButton = document.getElementById('logs-button');
const logsClose = document.getElementById('logs-close');
let logsPage = 0;
let logsCache = [];
let logsFilter = 'ALL';
let logsSort = 'newest';

async function fetchLogs(page = 0) {
  const perPage = parseInt(logsLimitSelect?.value || '25', 10);
  const fetchLimit = perPage * (page + 1);
  try {
    const res = await authFetch(`/logs?limit=${fetchLimit}`);
    if (!res.ok) throw new Error('logs fetch failed');
    const arr = await res.json();
    logsCache = Array.isArray(arr) ? arr : [];
    // Client-side filter by level
    if (logsFilter && logsFilter !== 'ALL') {
      logsCache = logsCache.filter(it => (it.level || '').toUpperCase() === logsFilter);
    }
    // Sort
    logsCache.sort((a, b) => {
      const ta = Number(a.ts_ms || 0);
      const tb = Number(b.ts_ms || 0);
      return logsSort === 'oldest' ? ta - tb : tb - ta;
    });
    renderLogsPage(page);
  } catch (e) {
    logsList.innerHTML = `<p class="empty-state">Unable to load logs: ${escapeHtml(e.message || '')}</p>`;
  }
}

function updateLogsSummary(page, perPage, total) {
  if (!logsSummary) return;
  if (!total) {
    logsSummary.textContent = '';
    return;
  }
  const start = page * perPage + 1;
  const end = Math.min(start + perPage - 1, total);
  logsSummary.textContent = `Showing ${start}–${end} of ${total}`;
}

function renderLogsPage(page = 0) {
  const perPage = parseInt(logsLimitSelect?.value || '25', 10);
  const start = page * perPage;
  const pageItems = logsCache.slice(start, start + perPage);
  if (!pageItems || pageItems.length === 0) {
    logsList.innerHTML = '<p class="logs-empty">No logs match this filter.</p>';
    if (logsPrev) logsPrev.disabled = true;
    if (logsNext) logsNext.disabled = true;
    updateLogsSummary(0, perPage, 0);
    return;
  }
  logsList.innerHTML = pageItems.map(item => {
    const d = new Date(Number(item.ts_ms || Date.now()));
    const ts = d.toLocaleString();
    const lvl = (item.level || 'INFO').toUpperCase();
    const msg = escapeHtml(item.msg || '');
    return (
      `<article class="log-row log-row-${lvl.toLowerCase()}">` +
      `<div class="log-row-head">` +
      `<span class="log-badge log-badge-${lvl.toLowerCase()}">${lvl}</span>` +
      `<time class="log-ts" datetime="${d.toISOString()}">${ts}</time>` +
      `</div>` +
      `<p class="log-msg">${msg}</p>` +
      `</article>`
    );
  }).join('');
  if (logsPrev) logsPrev.disabled = page === 0;
  if (logsNext) logsNext.disabled = (start + perPage) >= logsCache.length;
  logsPage = page;
  updateLogsSummary(page, perPage, logsCache.length);
}

// Wire controls
logsLimitSelect?.addEventListener('change', () => { logsPage = 0; fetchLogs(0); });
logsPrev?.addEventListener('click', () => { if (logsPage > 0) renderLogsPage(logsPage - 1); });
logsNext?.addEventListener('click', () => { renderLogsPage(logsPage + 1); });
logsSortSelect?.addEventListener('change', () => { logsSort = logsSortSelect.value || 'newest'; logsPage = 0; fetchLogs(0); });

document.querySelectorAll('#logs-filter-group button').forEach(btn => {
  btn.addEventListener('click', (ev) => {
    const v = ev.currentTarget.dataset.logFilter || 'ALL';
    logsFilter = v.toUpperCase();
    document.querySelectorAll('#logs-filter-group button').forEach(b => {
      const active = b === ev.currentTarget;
      b.classList.toggle('is-active', active);
      b.setAttribute('aria-selected', active ? 'true' : 'false');
    });
    logsPage = 0;
    fetchLogs(0);
  });
});

logsButton?.addEventListener('click', () => { if (!logsDialog?.showModal) return; logsDialog.showModal(); logsPage = 0; fetchLogs(0); });
logsClose?.addEventListener('click', () => logsDialog?.close());
logsDialog?.addEventListener('click', (event) => {
  if (event.target === logsDialog) logsDialog.close();
});


async function apiRequest(path, options = {}) {
  const response = await authFetch(path, options);
  if (response.ok) return response;
  const message = (await response.text()).trim();
  throw new Error(formatApiErrorMessage(message, response.status));
}

function formatApiErrorMessage(raw, status) {
  if (!raw) return `Request failed (${status})`;
  const text = String(raw).trim();
  try {
    const j = JSON.parse(text);
    if (j && typeof j.error === 'string') {
      const map = {
        invalid_package: 'The file is not a valid SmartApplet package (.os3kapp).',
        already_installed:
          'That applet is already installed on the Neo. Check “Replace an installed applet with the same ID”, then install again.',
        insufficient_space: 'Not enough free ROM or RAM on the Neo for this applet.',
        applet_too_large: 'The package is too large to upload (maximum 1 MB).',
        auto_backup_busy: 'Auto-backup is running. Try again when it finishes.',
        missing_body: 'No package data was received. Choose a file and try again.',
        upload_failed: 'The package upload failed. Check Wi‑Fi and try again.',
        out_of_memory: 'The buddy ran out of memory while receiving the package.',
        install_failed: j.detail ? `Install failed (${j.detail}).` : 'Install failed on the Neo.',
        fetch_failed: j.detail ? `Download failed (${j.detail}).` : 'Download failed.',
        applet_not_found: 'That applet was not found on the Neo.',
      };
      if (map[j.error]) return map[j.error];
      if (j.detail) return String(j.detail);
      return String(j.error).replace(/_/g, ' ');
    }
  } catch (_) { /* plain text */ }
  /* Never show raw JSON blobs in the UI. */
  if (text.startsWith('{') && text.includes('"error"')) {
    try {
      const j = JSON.parse(text);
      if (j?.error === 'already_installed') {
        return 'That applet is already installed on the Neo. Check “Replace an installed applet with the same ID”, then install again.';
      }
      if (typeof j?.error === 'string') return String(j.error).replace(/_/g, ' ');
    } catch (_) { /* ignore */ }
  }
  return text.length > 180 ? text.slice(0, 180) + '…' : text;
}

function setDialogStatus(el, message, type = '') {
  if (!el) return;
  el.textContent = message || '';
  if (type) el.dataset.type = type;
  else delete el.dataset.type;
}

function setButtonBusy(button, busy, busyLabel) {
  if (!button) return;
  const labelEl = button.querySelector('.btn-label');
  if (busy) {
    button.disabled = true;
    button.setAttribute('aria-busy', 'true');
    if (labelEl) {
      if (button.dataset.label == null) button.dataset.label = labelEl.textContent;
      /* Icon-only buttons keep their glyph; spinner comes from CSS ::before. */
      if (busyLabel && !button.classList.contains('icon-action-only')) {
        labelEl.textContent = busyLabel;
      }
    } else {
      if (button.dataset.label == null) button.dataset.label = button.textContent;
      button.textContent = busyLabel || button.dataset.label || 'Working…';
    }
    return;
  }
  button.disabled = false;
  button.removeAttribute('aria-busy');
  if (labelEl) {
    if (button.dataset.label != null) labelEl.textContent = button.dataset.label;
  } else if (button.dataset.label != null) {
    button.textContent = button.dataset.label;
  }
  delete button.dataset.label;
}

function showNotice(message, type = 'info') {
  const notice = document.querySelector('#neo-file-notice');
  notice.textContent = message;
  notice.dataset.type = type;
  notice.hidden = false;
}

// Format SD card handler (device settings)
// Format SD card flow: require typed confirmation in modal
document.getElementById('format-sd')?.addEventListener('click', (ev) => {
  ev.preventDefault();
  const dlg = document.getElementById('format-sd-dialog');
  if (dlg && dlg.showModal) {
    dlg.showModal();
    // fetch and show SD status when opening the dialog
    fetchSdStatus().catch(() => {});
  }
});

const fmtForm = document.getElementById('format-sd-form');
if (fmtForm) {
  const input = document.getElementById('format-confirm-input');
  const confirmBtn = document.getElementById('format-sd-confirm');
  const cancelBtns = [document.getElementById('format-sd-cancel'), document.getElementById('format-sd-cancel-2')];
  function closeFmt() {
    const d = document.getElementById('format-sd-dialog');
    if (d && d.close) d.close();
    input.value = '';
    confirmBtn.disabled = true;
    // stop any polling and reset UI
    stopSdStatusPolling();
    setGlobalUiDisabled(false);
    const statusEl = document.getElementById('sd-capacity-line'); if (statusEl) statusEl.style.display = 'none';
    const progressWrap = document.getElementById('sd-progress-wrap'); if (progressWrap) progressWrap.style.display = 'none';
  }
  cancelBtns.forEach(b => b?.addEventListener('click', () => closeFmt()));
  input?.addEventListener('input', () => {
    confirmBtn.disabled = (input.value.trim().toUpperCase() !== 'FORMAT');
  });
  fmtForm.addEventListener('submit', (ev) => {
    ev.preventDefault();
    if (input.value.trim().toUpperCase() !== 'FORMAT') return;
    (async () => {
      setButtonBusy(confirmBtn, true, 'Formatting...');
      // disable other UI during formatting
      setGlobalUiDisabled(true);
      let pollId = null;
      try {
        // initiate format
        await apiRequest('/sd/format', { method: 'POST' });
        // start polling status if available
        pollId = startSdStatusPolling();
        // wait until polling resolves (it will stop when formatting completes)
        await waitForSdFormatCompletion();
        showNotice('SD card formatted successfully.', 'success');
        closeFmt();
      } catch (e) {
        showNotice('SD format failed: ' + (e.message || ''), 'error');
      } finally {
        stopSdStatusPolling(pollId);
        setButtonBusy(confirmBtn, false);
        setGlobalUiDisabled(false);
      }
    })();
  });
}

document.getElementById('factory-reset-open')?.addEventListener('click', (ev) => {
  ev.preventDefault();
  const dlg = document.getElementById('factory-reset-dialog');
  const pw = document.getElementById('factory-reset-password');
  if (pw) pw.value = '';
  if (dlg?.showModal) dlg.showModal();
});

document.getElementById('settings-password-save')?.addEventListener('click', async () => {
  const current = document.getElementById('settings-password-current')?.value || '';
  const next = document.getElementById('settings-password-new')?.value || '';
  const confirm = document.getElementById('settings-password-confirm')?.value || '';
  if (!current || !next) {
    showNotice('Enter your current password and a new password.', 'error');
    return;
  }
  if (next.length < 8) {
    showNotice('New password must be at least 8 characters.', 'error');
    return;
  }
  if (next !== confirm) {
    showNotice('New passwords do not match.', 'error');
    return;
  }
  try {
    const res = await authFetch('/auth/password', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ current_password: current, new_password: next }),
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok) throw new Error(data.error || 'Could not change password.');
    document.getElementById('settings-password-current').value = '';
    document.getElementById('settings-password-new').value = '';
    document.getElementById('settings-password-confirm').value = '';
    showNotice('Portal password updated.', 'success');
  } catch (error) {
    showNotice(error.message, 'error');
  }
});

const factoryResetForm = document.getElementById('factory-reset-form');
if (factoryResetForm) {
  const pwInput = document.getElementById('factory-reset-password');
  const confirmBtn = document.getElementById('factory-reset-confirm');
  const cancelBtns = [document.getElementById('factory-reset-cancel'), document.getElementById('factory-reset-cancel-2')];
  function closeFactoryReset() {
    const dlg = document.getElementById('factory-reset-dialog');
    if (dlg?.close) dlg.close();
    if (pwInput) pwInput.value = '';
  }
  cancelBtns.forEach((btn) => btn?.addEventListener('click', closeFactoryReset));
  factoryResetForm.addEventListener('submit', (ev) => {
    ev.preventDefault();
    const password = pwInput?.value || '';
    if (!password) return;
    (async () => {
      setButtonBusy(confirmBtn, true, 'Resetting...');
      try {
        const res = await authFetch('/device/factory-reset', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ password }),
        });
        const data = await res.json().catch(() => ({}));
        if (!res.ok) throw new Error(data.error || 'Factory reset failed.');
        setAuthToken(null);
        localStorage.removeItem('neo2_setup_complete');
        localStorage.removeItem('neo2_portal_settings');
        localStorage.removeItem('neo2_device_name');
        updateSignInState();
        showNotice('Device is resetting. Reconnect to the buddy hotspot when it restarts.', 'success');
        closeFactoryReset();
        settingsDialog?.close();
      } catch (error) {
        showNotice(error.message, 'error');
      } finally {
        setButtonBusy(confirmBtn, false);
      }
    })();
  });
}

// --- SD status + formatting helpers ---
let sdStatusPollHandle = null;
let sdFormatCompletionResolver = null;

async function fetchSdStatus() {
  const statusEl = document.getElementById('sd-capacity-line');
  const capText = document.getElementById('sd-capacity-text');
  const progressWrap = document.getElementById('sd-progress-wrap');
  const progressEl = document.getElementById('sd-progress');
  const progressLabel = document.getElementById('sd-progress-label');
  try {
    const res = await authFetch('/sd/status');
    if (!res.ok) throw new Error('no status');
    const j = await res.json();
    if (j.size_bytes) {
      const pretty = humanFileSize(Number(j.size_bytes));
      capText.textContent = `${pretty}`;
      statusEl.style.display = '';
    } else {
      statusEl.style.display = 'none';
    }
    // If device reports an active formatting operation, show progress
      if (j.formatting) {
      progressWrap.style.display = '';
      const p = Number(j.progress || 0);
      progressEl.value = Math.max(0, Math.min(100, p));
      progressLabel.textContent = `${Math.round(progressEl.value)}%`;
    } else {
      progressWrap.style.display = 'none';
      progressEl.value = 0;
    }
    return j;
  } catch (e) {
    // ignore — server may not implement this endpoint
    statusEl && (statusEl.style.display = 'none');
    progressWrap && (progressWrap.style.display = 'none');
    throw e;
  }
}

function humanFileSize(bytes) {
  const thresh = 1024;
  if (Math.abs(bytes) < thresh) return bytes + ' B';
  const units = ['KiB','MiB','GiB','TiB'];
  let u = -1;
  do { bytes /= thresh; ++u; } while (Math.abs(bytes) >= thresh && u < units.length - 1);
  return bytes.toFixed(1) + ' ' + units[u];
}

function startSdStatusPolling(intervalMs = 1000) {
  stopSdStatusPolling();
  sdStatusPollHandle = setInterval(() => {
    fetchSdStatus().catch(() => {});
  }, intervalMs);
  return sdStatusPollHandle;
}

function stopSdStatusPolling(handle) {
  const h = handle || sdStatusPollHandle;
  if (h) clearInterval(h);
  sdStatusPollHandle = null;
}

function waitForSdFormatCompletion(timeoutMs = 120000) {
  // create a promise that resolves when device no longer reports formatting
  return new Promise((resolve, reject) => {
    const start = Date.now();
    sdFormatCompletionResolver = resolve;
    const check = async () => {
      try {
        const res = await authFetch('/sd/status');
        if (!res.ok) throw new Error('no status');
        const j = await res.json();
        if (!j.formatting) {
          resolve(j);
          sdFormatCompletionResolver = null;
          return;
        }
      } catch (e) {
        // continue polling even if status missing
      }
      if (Date.now() - start > timeoutMs) {
        reject(new Error('Formatting timed out'));
        sdFormatCompletionResolver = null;
        return;
      }
      setTimeout(check, 1000);
    };
    check();
  });
}

// Global UI disable during formatting: disable interactive controls outside the dialog
const globalDisabledStore = new Map();
function setGlobalUiDisabled(disabled) {
  // Targets: primary action buttons, settings controls, quick actions
  const selectors = 'button, input, select, textarea';
  document.querySelectorAll(selectors).forEach(el => {
    // skip elements inside the format dialog
    if (el.closest && el.closest('#format-sd-dialog')) return;
    // skip hidden elements
    if (el.hidden) return;
    if (disabled) {
      globalDisabledStore.set(el, el.disabled);
      el.disabled = true;
      el.setAttribute('aria-disabled', 'true');
    } else {
      if (globalDisabledStore.has(el)) {
        el.disabled = globalDisabledStore.get(el);
        globalDisabledStore.delete(el);
      } else {
        el.disabled = false;
      }
      el.removeAttribute('aria-disabled');
    }
  });
}

document.querySelectorAll('[data-action]').forEach((button) => button.addEventListener('click', () => {
  const action = button.dataset.action;
  if (action === 'settings') {
    openSettings();
    return;
  }
  if (action === 'wifi') {
    openSettings({ focusNetwork: true });
    return;
  }
  const [label, title, copy] = actions[action] || ['ACTION', 'Action', ''];
  document.querySelector('#dialog-label').textContent = label;
  document.querySelector('#dialog-title').textContent = title;
  document.querySelector('#dialog-copy').textContent = copy;

  document.querySelector('#dialog-actions-default').hidden = action === 'open-pairing' || action === 'open-ble-send';
  document.querySelector('#dialog-actions-pairing').hidden = action !== 'open-pairing';
  document.querySelector('#dialog-actions-ble-send').hidden = action !== 'open-ble-send';

  if (action === 'open-pairing') {
    refreshBleStatus();
  }
  if (action === 'open-ble-send') {
    document.querySelector('#ble-send-text').value = '';
    document.querySelector('#ble-preview-box').hidden = true;
    document.querySelector('#ble-send-status').textContent = '';
    blePreviewLength = 0;
    refreshBleStatus();
  }
  dialog.showModal();
}));

document.querySelector('#action-dialog-close')?.addEventListener('click', () => {
  stopBlePairingPoll();
  dialog.close();
});
document.querySelector('#action-dialog-form')?.addEventListener('submit', (event) => {
  if (event.submitter?.id !== 'action-dialog-close') return;
  event.preventDefault();
  stopBlePairingPoll();
  dialog.close();
});

let blePairingPoll = null;

function stopBlePairingPoll() {
  if (blePairingPoll) {
    clearInterval(blePairingPoll);
    blePairingPoll = null;
  }
}

function bleStatusLabel(j) {
  if (j.state === 'connected') {
    return 'Connected — Neo keys pass through live';
  }
  if (j.state === 'pairing' || j.pairing_enabled) {
    if (j.advertising) {
      return 'Advertising now — choose Neo2 Buddy and tap Pair (no PIN, 3 min)';
    }
    if (j.ready) {
      return 'Bluetooth starting — advertising should begin in a moment…';
    }
    return 'Pairing requested, but Bluetooth is not advertising yet';
  }
  if (j.bonded > 0) {
    return `Idle — ${j.bonded} bonded host${j.bonded === 1 ? '' : 's'}, waiting to reconnect`;
  }
  return 'Bluetooth off — start pairing to add a host';
}

async function refreshBleStatus() {
  const line = document.querySelector('#ble-status-line');
  const bondsList = document.querySelector('#ble-bonds-list');
  if (!line) return;
  try {
    const res = await authFetch('/ble');
    if (!res.ok) throw new Error('unavailable');
    const j = await res.json();
    const sendLine = document.querySelector('#ble-send-status');
    line.textContent = `${bleStatusLabel(j)}. ${j.can_send ? 'Host ready for Neo typing and portal Send text.' : 'Connect or pair a Bluetooth host to type.'}`;
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
      if (!blePairingPoll) {
        blePairingPoll = setInterval(() => { refreshBleStatus(); }, 1500);
      }
    } else {
      stopBlePairingPoll();
    }
    if (sBleConnected && sendLine) {
      sendLine.textContent = 'While Bluetooth is connected, ASM (manager) mode on the Documents page may stop keystrokes until keyboard mode returns.';
    } else if (sendLine) {
      sendLine.textContent = j.can_send
        ? 'Host ready. Neo keys pass through; portal text send is optional.'
        : 'Waiting for Bluetooth host connection...';
    }
  } catch (error) {
    line.textContent = 'Bluetooth status unavailable.';
    if (bondsList) {
      bondsList.hidden = true;
      bondsList.innerHTML = '';
    }
  }
}

document.querySelector('#ble-start-pairing')?.addEventListener('click', async () => {
  const line = document.querySelector('#ble-status-line');
  if (line) line.textContent = 'Starting Bluetooth…';
  try {
    const res = await authFetch('/ble/pairing', { method: 'POST', body: JSON.stringify({ enabled: true }), headers: { 'Content-Type': 'application/json' } });
    const j = await res.json().catch(() => ({}));
    if (!res.ok) {
      throw new Error(j.error || 'Pairing could not be started.');
    }
    if (j.advertising) {
      showNotice('Device is advertising — choose Neo2 Buddy and tap Pair (no PIN).', 'success');
    } else {
      showNotice('Bluetooth started — waiting for advertising…', 'success');
    }
    refreshBleStatus();
  } catch (error) {
    stopBlePairingPoll();
    if (line) line.textContent = error.message || 'Pairing could not be started.';
    showNotice(error.message, 'error');
  }
});

document.querySelector('#ble-stop-pairing')?.addEventListener('click', async () => {
  try {
    await authFetch('/ble/pairing', { method: 'POST', body: JSON.stringify({ enabled: false }), headers: { 'Content-Type': 'application/json' } });
    stopBlePairingPoll();
    showNotice('Pairing stopped.', 'success');
    refreshBleStatus();
  } catch (error) {
    showNotice(error.message, 'error');
  }
});

document.querySelector('#ble-clear-bonds')?.addEventListener('click', async () => {
  const line = document.querySelector('#ble-status-line');
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

document.querySelector('#ble-preview-btn')?.addEventListener('click', async () => {
  const text = document.querySelector('#ble-send-text').value;
  const status = document.querySelector('#ble-send-status');
  const previewBox = document.querySelector('#ble-preview-box');
  if (!text.trim()) {
    status.textContent = 'Enter text to preview.';
    return;
  }
  try {
    const res = await authFetch('/ble/preview', { method: 'POST', body: JSON.stringify({ text }), headers: { 'Content-Type': 'application/json' } });
    if (!res.ok) throw new Error('Preview failed.');
    const j = await res.json();
    blePreviewLength = j.length || text.length;
    previewBox.hidden = false;
    previewBox.textContent = j.preview || text.slice(0, 200);
    status.textContent = `Ready to send ${blePreviewLength} character${blePreviewLength === 1 ? '' : 's'}. ${j.can_send ? '' : 'Connect a Bluetooth host first.'}`;
  } catch (error) {
    status.textContent = error.message;
  }
});

document.querySelector('#ble-confirm-send')?.addEventListener('click', async () => {
  const status = document.querySelector('#ble-send-status');
  if (!blePreviewLength) {
    status.textContent = 'Preview the text before sending.';
    return;
  }
  if (!confirm(`Send ${blePreviewLength} characters as keystrokes to the connected host?`)) return;
  try {
    const res = await authFetch('/ble/send', { method: 'POST', body: '{}', headers: { 'Content-Type': 'application/json' } });
    if (res.status === 412) throw new Error('No Bluetooth host connected or preview expired.');
    if (!res.ok) throw new Error('Send failed.');
    status.textContent = 'Sending keystrokes...';
    showNotice('Bluetooth transfer started.', 'success');
  } catch (error) {
    status.textContent = error.message;
  }
});

document.querySelector('#ble-cancel-send')?.addEventListener('click', async () => {
  try {
    await authFetch('/ble/cancel', { method: 'POST', body: '{}', headers: { 'Content-Type': 'application/json' } });
    blePreviewLength = 0;
    document.querySelector('#ble-preview-box').hidden = true;
    document.querySelector('#ble-send-status').textContent = 'Transfer cancelled.';
  } catch (error) {
    document.querySelector('#ble-send-status').textContent = error.message;
  }
});

function syncProviderFieldsUi() {
  const provider = document.querySelector('input[name="provider"]:checked')?.value || 'webdav';
  const bucketField = document.querySelector('#bucket-field');
  const regionField = document.querySelector('#region-field');
  const pathHint = document.querySelector('#sync-path-hint');
  const endpoint = document.querySelector('#sync-endpoint');
  const pathInput = document.querySelector('#sync-path');
  const userLabel = document.querySelector('#user-field');
  const secretLabel = document.querySelector('#secret-field');
  if (bucketField) bucketField.hidden = provider !== 's3';
  if (regionField) regionField.hidden = provider !== 's3';
  if (pathHint) {
    pathHint.textContent = provider === 'hammer'
      ? 'Hammer project name (created on first sync if missing).'
      : 'Optional subfolder for uploaded backups.';
  }
  if (pathInput) {
    pathInput.placeholder = provider === 'hammer' ? 'Neo2 Buddy' : 'alpha-smart/neo2';
  }
  if (endpoint) {
    endpoint.placeholder = provider === 'hammer'
      ? 'https://hammer.ink'
      : provider === 's3'
        ? 'https://ACCOUNT_ID.r2.cloudflarestorage.com'
        : 'https://cloud.example.com/remote.php/dav/files/you/backups';
    if (provider === 'hammer' && (!endpoint.value || /example\.com|r2\.cloudflare/i.test(endpoint.value))) {
      endpoint.value = 'https://hammer.ink';
    }
  }
  if (userLabel) {
    const input = userLabel.querySelector('input');
    userLabel.childNodes[0].textContent = provider === 'hammer' ? 'Email' : 'Username or access key';
    if (input) input.placeholder = provider === 'hammer' ? 'you@example.com' : '';
  }
  if (secretLabel) {
    secretLabel.childNodes[0].textContent = provider === 'hammer' ? 'Password' : 'App password or secret key';
  }
  updateSyncProviderHelp(provider);
}

function updateSyncProviderHelp(provider) {
  const el = document.querySelector('#sync-provider-help');
  if (!el) return;
  if (provider === 's3') {
    el.innerHTML = '<strong>S3-compatible setup</strong><br>Use the bucket endpoint (for example Cloudflare R2 or AWS S3). Set <strong>Bucket</strong>, <strong>Region</strong> (<code>auto</code> for R2), access key ID as username, and secret access key. The buddy must be on home Wi‑Fi so the clock can sync before uploads.';
    return;
  }
  if (provider === 'hammer') {
    el.innerHTML = '<strong>Hammer Ink setup</strong><br>Use your <a href="https://hammer.ink/" target="_blank" rel="noopener">hammer.ink</a> account email and password. Backups are uploaded as <strong>Notes</strong> into a Hammer project (default name <code>Neo2 Buddy</code>). Official server access may require a Patreon subscription. Local files stay on the buddy.';
    return;
  }
  el.innerHTML = '<strong>WebDAV setup</strong><br>For Nextcloud, paste the full WebDAV folder URL (often ending in <code>/remote.php/dav/files/you/backups</code>). Use your account username and an app password — not your login password. The buddy creates the optional subfolder automatically before the first upload.';
}

function validateCloudSyncPayload(payload, credentialsConfigured) {
  if (!payload.endpoint || !payload.endpoint.startsWith('https://')) {
    throw new Error('Server URL must start with https://');
  }
  if (!payload.username) {
    throw new Error(payload.provider === 'hammer' ? 'Email is required.' : 'Username or access key is required.');
  }
  if (payload.provider === 's3' && !payload.bucket) {
    throw new Error('Bucket name is required for S3-compatible storage.');
  }
  if (!credentialsConfigured && !payload.secret) {
    throw new Error(payload.provider === 'hammer'
      ? 'Enter your hammer.ink password the first time you save.'
      : 'Enter an app password or secret key the first time you save.');
  }
}

function updateCloudSyncHealth(cfg) {
  const healthEl = document.querySelector('#sync-health');
  const subtitle = document.querySelector('#open-sync-subtitle');
  const checklist = document.querySelector('#sync-checklist');
  const health = cfg?.health || {};
  const state = health.state || (cfg?.enabled ? 'warning' : 'idle');
  const issues = Array.isArray(health.issues) ? health.issues : [];

  if (healthEl) {
    healthEl.dataset.state = state;
    if (!cfg?.enabled) {
      healthEl.textContent = 'Cloud upload is disabled. Enable it after you save a destination.';
    } else if (health.ready) {
      healthEl.textContent = 'Cloud backup is ready. Run Test connection before your first upload.';
    } else if (issues.length > 0) {
      healthEl.textContent = issues[0];
    } else if (health.configured) {
      healthEl.textContent = 'Destination saved on device. Run Test connection to verify access.';
    } else {
      healthEl.textContent = 'Finish the fields below, save, then run Test connection.';
    }
  }

  if (subtitle) {
    if (!cfg?.enabled) subtitle.textContent = 'Disabled — tap to configure';
    else if (state === 'ok') subtitle.textContent = 'Ready for cloud upload';
    else if (state === 'error') subtitle.textContent = 'Cloud upload needs attention';
    else if (health.configured) subtitle.textContent = 'Saved — test connection recommended';
    else subtitle.textContent = 'WebDAV, S3, or Hammer Ink';
  }

  if (checklist) {
    const setOk = (name, value) => {
      const item = checklist.querySelector(`[data-check="${name}"]`);
      if (item) item.dataset.ok = value ? 'true' : 'false';
    };
    setOk('wifi', !!health.wifi_ok);
    const provider = cfg?.provider || 'webdav';
    const clockItem = checklist.querySelector('[data-check="clock"]');
    if (clockItem) {
      if (provider === 's3') clockItem.dataset.ok = health.clock_ok ? 'true' : 'false';
      else clockItem.dataset.ok = 'true';
      if (provider === 'hammer') clockItem.textContent = 'Hammer account can sign in';
      else clockItem.textContent = 'Device clock synced (required for S3)';
    }
    setOk('saved', !!health.configured);
    const st = cfg?.status || {};
    if (st.last_test_ok) setOk('test', true);
    else if (st.last_test_message) setOk('test', false);
    else setOk('test', false);
  }
}

async function loadCloudSyncConfig() {
  const form = document.querySelector('#sync-form');
  if (!form || !getAuthToken()) return null;
  try {
    const res = await authFetch('/sync/config');
    if (!res.ok) return null;
    const cfg = await res.json();
    const provider = cfg.provider || 'none';
    const providerValue = (provider === 's3' || provider === 'hammer') ? provider : 'webdav';
    const radio = document.querySelector(`input[name="provider"][value="${providerValue}"]`);
    if (radio) radio.checked = true;
    form.elements.endpoint.value = cfg.endpoint || '';
    form.elements.path.value = cfg.folder || cfg.path || '';
    if (form.elements.bucket) form.elements.bucket.value = cfg.bucket || '';
    if (form.elements.region) form.elements.region.value = cfg.region || 'us-east-1';
    form.elements.username.value = cfg.username || '';
    if (form.elements.enabled) form.elements.enabled.checked = !!cfg.enabled;
    form.elements.secret.value = '';
    form.dataset.credentialsConfigured = cfg.credentials_configured ? '1' : '0';
    syncProviderFieldsUi();
    updateSyncStateText(cfg);
    updateCloudSyncHealth(cfg);
    return cfg;
  } catch (e) {
    return null;
  }
}

function updateSyncStateText(cfg) {
  const el = document.querySelector('#sync-state');
  if (!el) return;
  const st = cfg?.status;
  if (st?.busy) {
    const cur = st.current || 0;
    const total = st.total || 0;
    el.textContent = total > 0
      ? `Uploading ${cur}/${total}… (${st.uploaded || 0} ok, ${st.failed || 0} failed)`
      : 'Starting cloud upload…';
    return;
  }
  if (st?.last_test_message && !st?.busy) {
    const testLine = st.last_test_ok ? `Last test: ${st.last_test_message}` : `Last test failed: ${st.last_test_message}`;
    if (st?.last_message && !st.last_ok) {
      el.textContent = `${testLine}. Last upload: ${st.last_message}`;
      return;
    }
    el.textContent = testLine;
    return;
  }
  if (st?.last_message) {
    el.textContent = st.last_ok ? st.last_message : `Last upload: ${st.last_message}`;
    return;
  }
  if (cfg?.credentials_configured) {
    el.textContent = cfg.enabled
      ? 'Cloud destination saved on device. Local backups are never deleted after upload.'
      : 'Credentials saved but upload is disabled. Enable to use Cloud upload.';
  } else {
    el.textContent = 'Credentials are stored on the device only. Secrets are never shown again after save.';
  }
}

let syncPollTimer = null;
function startSyncPoll() {
  if (syncPollTimer) return;
  syncPollTimer = setInterval(async () => {
    try {
      const res = await authFetch('/sync/config');
      if (!res.ok) return;
      const cfg = await res.json();
      updateSyncStateText(cfg);
      updateCloudSyncHealth(cfg);
      if (!cfg.status?.busy) {
        clearInterval(syncPollTimer);
        syncPollTimer = null;
      }
    } catch (e) {
      clearInterval(syncPollTimer);
      syncPollTimer = null;
    }
  }, 1500);
}

async function saveCloudSyncConfig(form) {
  const data = new FormData(form);
  const payload = {
    provider: data.get('provider') || 'webdav',
    enabled: !!form.elements.enabled?.checked,
    endpoint: (data.get('endpoint') || '').trim(),
    path: (data.get('path') || '').trim(),
    bucket: (data.get('bucket') || '').trim(),
    region: (data.get('region') || '').trim() || 'us-east-1',
    username: (data.get('username') || '').trim(),
  };
  const secret = (data.get('secret') || '').trim();
  if (secret) payload.secret = secret;
  validateCloudSyncPayload(payload, form.dataset.credentialsConfigured === '1');
  const res = await authFetch('/sync/config', {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  });
  if (!res.ok) {
    const err = await res.json().catch(() => ({}));
    throw new Error(err.error || 'Could not save cloud settings.');
  }
  form.elements.secret.value = '';
  form.dataset.credentialsConfigured = '1';
  await loadCloudSyncConfig();
}

async function testCloudSync(form) {
  const el = document.querySelector('#sync-state');
  if (el) el.textContent = 'Saving settings, then testing connection…';
  if (form) await saveCloudSyncConfig(form);
  if (el) el.textContent = 'Testing connection…';
  const res = await authFetch('/sync/test', { method: 'POST', body: '{}', headers: { 'Content-Type': 'application/json' } });
  const data = await res.json().catch(() => ({}));
  await loadCloudSyncConfig();
  if (el) el.textContent = data.message || (res.ok ? 'Test OK' : 'Test failed');
  if (!res.ok) throw new Error(data.message || 'Test failed');
}

async function runCloudSync() {
  const el = document.querySelector('#sync-state');
  if (el) el.textContent = 'Starting upload…';
  const res = await authFetch('/sync/run', { method: 'POST', body: '{}', headers: { 'Content-Type': 'application/json' } });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || 'Cloud upload could not start');
  startSyncPoll();
}

function showFirstTimeSetup() {
  if (!localStorage.getItem('neo2_setup_complete')) setupDialog.showModal();
}

function portalSettings() {
  return JSON.parse(localStorage.getItem('neo2_portal_settings') || '{}');
}

/** @type {{ssid: string, password_set?: boolean, preferred?: boolean, password?: string}[]} */
let savedWifiNetworks = [];

function renderSavedWifiList() {
  const list = document.getElementById('wifi-saved-list');
  if (!list) return;
  if (!savedWifiNetworks.length) {
    list.innerHTML = '<li class="wifi-saved-empty">No saved home networks yet. Scan, enter the password, then Add / update.</li>';
    return;
  }
  list.innerHTML = savedWifiNetworks
    .map((n) => {
      const pref = !!n.preferred;
      const meta = pref ? 'preferred' : n.password_set || n.password ? 'saved' : 'needs password';
      return `<li class="wifi-saved-item${pref ? ' is-preferred' : ''}" data-ssid="${escapeHtml(n.ssid)}">
        <button type="button" class="button small wifi-saved-prefer" data-ssid="${escapeHtml(n.ssid)}">${escapeHtml(n.ssid)}</button>
        <span class="wifi-saved-meta">${meta}</span>
        <button type="button" class="button small danger-action wifi-saved-remove" data-ssid="${escapeHtml(n.ssid)}" aria-label="Remove ${escapeHtml(n.ssid)}">Remove</button>
      </li>`;
    })
    .join('');
}

function setPreferredWifi(ssid) {
  if (!ssid) return;
  savedWifiNetworks.forEach((n) => {
    n.preferred = n.ssid === ssid;
  });
  const sel = document.querySelector('#wifi-ssid');
  if (sel) {
    sel.dataset.savedSsid = ssid;
    if (![...sel.options].some((o) => o.value === ssid)) {
      sel.insertAdjacentHTML('beforeend', `<option value="${escapeHtml(ssid)}">${escapeHtml(ssid)} (saved)</option>`);
    }
    sel.value = ssid;
  }
  renderSavedWifiList();
}

function upsertSavedWifi(ssid, password) {
  if (!ssid) return false;
  let entry = savedWifiNetworks.find((n) => n.ssid === ssid);
  if (!entry) {
    if (savedWifiNetworks.length >= 4) {
      showNotice('You can save up to 4 Wi‑Fi networks.', 'error');
      return false;
    }
    entry = { ssid, password_set: false };
    savedWifiNetworks.push(entry);
  }
  if (password) {
    entry.password = password;
    entry.password_set = true;
  }
  savedWifiNetworks.forEach((n) => {
    n.preferred = n.ssid === ssid;
  });
  setPreferredWifi(ssid);
  return true;
}

function removeSavedWifi(ssid) {
  savedWifiNetworks = savedWifiNetworks.filter((n) => n.ssid !== ssid);
  if (savedWifiNetworks.length && !savedWifiNetworks.some((n) => n.preferred)) {
    savedWifiNetworks[0].preferred = true;
    setPreferredWifi(savedWifiNetworks[0].ssid);
  } else if (!savedWifiNetworks.length) {
    const sel = document.querySelector('#wifi-ssid');
    if (sel) {
      sel.dataset.savedSsid = '';
      sel.value = '';
    }
    renderSavedWifiList();
  } else {
    renderSavedWifiList();
  }
}

function settingsNetworkMode() {
  const checked = document.querySelector('input[name="settings-network-mode"]:checked');
  return checked ? checked.value : 'direct';
}

function syncSettingsNetworkModeUi() {
  const home = settingsNetworkMode() === 'home';
  const directFields = document.getElementById('settings-direct-fields');
  const homeFields = document.getElementById('settings-home-fields');
  if (directFields) directFields.hidden = home;
  if (homeFields) homeFields.hidden = !home;
  const note = document.querySelector('#settings-note');
  if (note && getAuthToken()) {
    note.textContent = home
      ? 'Choose your home network and save. The buddy restarts and joins your router.'
      : 'Update the hotspot name or password and save. The buddy restarts in Direct access mode.';
  }
}

async function openSettings(options = {}) {
  const settings = portalSettings();
  const deviceName = settings.deviceName || localStorage.getItem('neo2_device_name') || 'Neo2 Buddy';
  document.querySelector('#settings-device-name').value = deviceName;
  document.querySelector('#settings-device-title').textContent = deviceName;
  document.querySelector('#settings-sleep').value = settings.sleep || '10';
  document.querySelector('#settings-private-live').checked = settings.privateLive !== false;
  document.querySelector('#settings-prefer-sd').checked = settings.preferSd !== false;
  toggleStaticIpFields();
  document.querySelector('#settings-note').textContent = 'Loading device settings…';
  try {
    const res = await authFetch('/settings');
    if (res.ok) {
      const sj = await res.json();
      document.querySelector('#settings-require-auth').checked = !!sj.require_portal_auth;
      const autoBak = document.querySelector('#settings-auto-backup');
      if (autoBak) syncAutoBackupToggles(!!sj.auto_backup_on_connect, { fromEl: autoBak });
      const autoCloud = document.querySelector('#settings-auto-cloud');
      if (autoCloud) autoCloud.checked = !!sj.auto_cloud_sync_after_backup;
      const neoLabel = document.querySelector('#settings-neo-label');
      if (neoLabel) neoLabel.value = sj.neo_label || '';
      if (sj.device_name) {
        document.querySelector('#settings-device-name').value = sj.device_name;
        document.querySelector('#settings-device-title').textContent = sj.device_name;
      }
      if (sj.keyboard_layout) {
        document.querySelector('#settings-keyboard-layout').value = sj.keyboard_layout;
      }
      if (sj.sleep_timeout_seconds) {
        document.querySelector('#settings-sleep').value = String(sj.sleep_timeout_seconds);
      }
      const mode = sj.network_mode === 'home' ? 'home' : 'direct';
      const modeInput = document.querySelector(`input[name="settings-network-mode"][value="${mode}"]`);
      if (modeInput) modeInput.checked = true;
      document.querySelector('#hotspot-ssid').value = sj.hotspot_ssid || '';
      document.querySelector('#hotspot-password').value = '';
      const ssidSelect = document.querySelector('#wifi-ssid');
      if (Array.isArray(sj.wifi_networks) && sj.wifi_networks.length) {
        savedWifiNetworks = sj.wifi_networks
          .filter((n) => n && n.ssid)
          .map((n) => ({
            ssid: n.ssid,
            password_set: !!n.password_set,
            preferred: !!n.preferred || n.ssid === sj.wifi_ssid,
          }));
      } else if (sj.wifi_ssid) {
        savedWifiNetworks = [{ ssid: sj.wifi_ssid, password_set: true, preferred: true }];
      } else {
        savedWifiNetworks = [];
      }
      if (ssidSelect) {
        ssidSelect.dataset.savedSsid = sj.wifi_ssid || (savedWifiNetworks[0] && savedWifiNetworks[0].ssid) || '';
        ssidSelect.value = ssidSelect.dataset.savedSsid || '';
      }
      renderSavedWifiList();
      document.querySelector('#wifi-password').value = '';
      const dhcpEl = document.querySelector('#wifi-dhcp');
      if (dhcpEl) dhcpEl.checked = sj.wifi_dhcp !== false;
      document.querySelector('#wifi-ip').value = sj.wifi_ip || '';
      document.querySelector('#wifi-netmask').value = sj.wifi_netmask || '';
      document.querySelector('#wifi-gateway').value = sj.wifi_gateway || '';
      document.querySelector('#wifi-dns').value = sj.wifi_dns || '';
      toggleStaticIpFields();
      document.querySelector('#settings-note').textContent = 'Changes to network mode or Wi‑Fi settings restart the buddy in about two seconds.';
      if (getAuthToken()) {
        try {
          const wres = await authFetch('/wifi');
          if (wres.ok) {
            const wj = await wres.json();
            const statusEl = document.getElementById('network-mode-status');
            if (statusEl) {
              const modeLabel = wj.network_mode === 'home' ? 'Home network' : 'Direct access';
              if (wj.network_mode === 'home' && wj.ip) {
                statusEl.textContent = `Active now: ${modeLabel} — ${wj.ssid || 'connected'} at ${wj.ip}.`;
              } else if (wj.network_mode === 'home') {
                statusEl.textContent = `Active now: ${modeLabel} — joining ${wj.ssid || 'saved network'}…`;
              } else {
                statusEl.textContent = `Active now: ${modeLabel}${wj.hotspot_ssid ? ` — hotspot “${wj.hotspot_ssid}”` : ''} at 192.168.4.1.`;
              }
            }
          }
        } catch (e) {
          // ignore
        }
      }
    } else if (getAuthToken()) {
      document.querySelector('#settings-note').textContent = 'Could not load device settings. Network changes may not apply.';
    } else {
      document.querySelector('#settings-note').textContent = 'Sign in to load and save device settings.';
    }
  } catch (e) {
    document.querySelector('#settings-note').textContent = 'Could not reach the device settings API.';
  }
  syncSettingsNetworkModeUi();
  if (getAuthToken()) fetchWifiScan();
  settingsDialog.showModal();
  if (options.focusNetwork) {
    const section = document.getElementById('wifi-section');
    if (section) {
      requestAnimationFrame(() => section.scrollIntoView({ behavior: 'smooth', block: 'start' }));
    }
  }
}

function initHeaderFooter() {
  try {
    const appMeta = document.querySelector('meta[name="application-name"]')?.getAttribute('content') || 'Neo2 Buddy Portal';
    const appVer = document.querySelector('meta[name="application-version"]')?.getAttribute('content') || '';
    const relDate = document.querySelector('meta[name="release-date"]')?.getAttribute('content') || '';
    const appNameEl = document.getElementById('app-name');
    const headerDeviceEl = document.getElementById('header-device-name');
    const footerApp = document.getElementById('footer-app-name');
    const footerVer = document.getElementById('footer-version');
    const footerRel = document.getElementById('footer-release');
    if (appNameEl) appNameEl.textContent = appMeta;
    if (footerApp) footerApp.textContent = appMeta;
    if (footerVer && appVer) footerVer.textContent = `v${appVer}`;
    if (footerRel && relDate) footerRel.textContent = `(${relDate})`;
    const deviceName = localStorage.getItem('neo2_device_name') || 'Neo2 Buddy';
    if (headerDeviceEl) headerDeviceEl.textContent = deviceName;
  } catch (e) {
    // ignore
  }
}

function updateSignInState() {
  const btn = document.getElementById('login-btn');
  if (btn) {
    const authed = isSessionAuthed();
    btn.textContent = authed ? 'Sign out' : 'Sign In';
    btn.title = authed ? 'Sign out of the portal' : 'Sign in to manage this device';
    btn.setAttribute('aria-pressed', authed ? 'true' : 'false');
  }
  updateAuthVisibility();
}

/** IDs that must stay hidden until a portal session exists. */
const AUTH_REQUIRED_IDS = [
  'logs-button',
  'settings-button',
  'refresh-files',
  'upload-file',
  'open-sync',
  'sync-run-backups',
  'refresh-neo-files',
  'neo-charmap',
  'neo-read-all',
  'neo-backup-now',
  'neo-manager',
  'neo-rescan',
  'install-applet',
  'install-from-store',
  'refresh-applets',
  'remove-all-applets',
  'dashboard-auto-backup-wrap',
  'backups-auto-backup-wrap',
  'open-wifi',
  'pause-live',
  'toggle-raw',
  'clear-live',
  'follow-indicator',
];

function clearAuthenticatedUi() {
  const conn = document.getElementById('connection');
  if (conn) conn.textContent = 'Not signed in';

  sNeoKeyboardActive = false;
  sNeoCommsReady = false;
  sBleConnected = false;
  sBleState = 'idle';
  sBackupBusy = false;

  const neoConn = document.getElementById('neo-connection');
  if (neoConn) neoConn.textContent = 'Sign in';
  const neoDetail = document.getElementById('neo-connection-detail');
  if (neoDetail) neoDetail.textContent = 'Sign in to see Neo USB status and run Scan / Backup.';

  const model = document.getElementById('neo-model');
  if (model) model.textContent = '—';
  const modelDetail = document.getElementById('neo-model-detail');
  if (modelDetail) modelDetail.textContent = 'Available after sign-in';

  const appletCount = document.getElementById('applet-count');
  if (appletCount) appletCount.textContent = '—';
  const appletDetail = document.getElementById('applet-count-detail');
  if (appletDetail) appletDetail.textContent = 'Sign in, then Refresh SmartApplets';

  const backupCount = document.getElementById('backup-count');
  if (backupCount) backupCount.textContent = '—';
  const backupDetail = document.getElementById('backup-count-detail');
  if (backupDetail) backupDetail.textContent = 'Sign in to list local backups';

  if (fileList) {
    fileList.innerHTML = '';
    fileList.hidden = true;
  }
  if (filesEmpty) {
    filesEmpty.hidden = false;
    const h = filesEmpty.querySelector('h3');
    const p = filesEmpty.querySelector('p');
    if (h) h.textContent = 'Sign in to view backups';
    if (p) p.textContent = 'Local backup files appear here after you sign in.';
  }

  if (appletList) {
    appletList.innerHTML = '';
    appletList.hidden = true;
  }
  if (appletsEmpty) {
    appletsEmpty.hidden = false;
    const h = appletsEmpty.querySelector('h3');
    const p = appletsEmpty.querySelector('p');
    if (h) h.textContent = 'Sign in to manage applets';
    if (p) p.textContent = 'Installed SmartApplets appear here after you sign in and tap Refresh.';
  }

  if (neoFileList) neoFileList.innerHTML = '';
  if (neoFileTableWrap) neoFileTableWrap.hidden = true;
  if (neoFilesEmpty) {
    neoFilesEmpty.hidden = false;
    const h = neoFilesEmpty.querySelector('h3');
    const p = neoFilesEmpty.querySelector('p');
    if (h) h.textContent = 'Sign in to scan Neo documents';
    if (p) p.textContent = 'Connect the Neo and sign in, then use Scan NEO.';
  }

  const guidance = document.querySelector('#neo-documents-guidance');
  if (guidance) {
    guidance.textContent = 'Sign in to scan, backup, and manage AlphaWord documents on the connected Neo.';
  }

  const liveText = document.getElementById('live-text');
  if (liveText) {
    liveText.textContent = 'Sign in to monitor live Neo keyboard input.';
  }

  flashDeckList = [];
  flashDeckActiveId = null;
  flashDeckDraft = null;
  const flashEmpty = document.querySelector('#flashdeck-empty');
  const flashLayout = document.querySelector('#flashdeck-layout');
  if (flashLayout) flashLayout.hidden = true;
  if (flashEmpty) {
    flashEmpty.hidden = false;
    const h = flashEmpty.querySelector('h3');
    const p = flashEmpty.querySelector('p');
    if (h) h.textContent = 'No decks yet';
    if (p) p.textContent = 'Create a set or import a text file.';
  }
  const flashList = document.querySelector('#flashdeck-set-list');
  if (flashList) flashList.innerHTML = '';
  const flashCards = document.querySelector('#flashdeck-cards');
  if (flashCards) flashCards.innerHTML = '';
  document.getElementById('stock-store-dialog')?.close();
  document.getElementById('flashdeck-dialog')?.close();

  const notices = document.getElementById('portal-notices');
  if (notices) {
    notices.hidden = true;
    notices.innerHTML = '';
  }

  try {
    sessionStorage.removeItem('neo_applet_count');
  } catch (_) { /* ignore */ }
}

function updateAuthVisibility() {
  const authed = isSessionAuthed();
  document.body.classList.toggle('portal-authed', authed);
  document.body.classList.toggle('portal-guest', !authed);

  for (const id of AUTH_REQUIRED_IDS) {
    const el = document.getElementById(id);
    if (el) el.hidden = !authed;
  }
  document.querySelectorAll('[data-auth-required]').forEach((el) => {
    el.hidden = !authed;
  });

  if (!authed) {
    clearAuthenticatedUi();
  } else {
    const conn = document.getElementById('connection');
    if (conn && (!conn.textContent || conn.textContent === 'Not signed in')) {
      conn.textContent = 'Signed in…';
    }
    const guidance = document.querySelector('#neo-documents-guidance');
    if (guidance && guidance.textContent.startsWith('Sign in')) {
      guidance.innerHTML =
        'Scan, read, write, and backup switch Neo to ASM (manager) mode only when you click them — not in the background. Only <strong>AlphaWord</strong> documents (8 slots) are shown and backed up. <strong>Backup now</strong> saves changed files only (same as Auto on connect). <strong>Backup all</strong> rewrites every non-empty slot. If Bluetooth is connected, keystrokes to your paired device may stop until keyboard mode returns.';
    }
    if (neoFilesEmpty) {
      const h = neoFilesEmpty.querySelector('h3');
      const p = neoFilesEmpty.querySelector('p');
      if (h && h.textContent.startsWith('Sign in')) h.textContent = 'NEO files will appear here';
      if (p && p.textContent.includes('sign in')) {
        p.textContent = 'Connect the NEO by USB, then scan it to list AlphaWord documents on the device.';
      }
    }
    if (appletsEmpty) {
      const h = appletsEmpty.querySelector('h3');
      const p = appletsEmpty.querySelector('p');
      if (h && h.textContent.startsWith('Sign in')) h.textContent = 'Connect your NEO';
      if (p && p.textContent.includes('sign in')) {
        p.textContent = 'Installed applets will appear here after the NEO USB connection is active.';
      }
    }
    if (filesEmpty) {
      const h = filesEmpty.querySelector('h3');
      const p = filesEmpty.querySelector('p');
      if (h && h.textContent.startsWith('Sign in')) h.textContent = 'No saved documents';
      if (p && p.textContent.includes('sign in')) {
        p.textContent = 'Read a file from the NEO to create a local backup on SD or internal storage.';
      }
    }
    const modelDetail = document.getElementById('neo-model-detail');
    if (modelDetail && modelDetail.textContent === 'Available after sign-in') {
      modelDetail.textContent = 'Reported by the NEO when connected';
    }
    const appletDetail = document.getElementById('applet-count-detail');
    if (appletDetail && appletDetail.textContent.startsWith('Sign in')) {
      appletDetail.textContent = 'Use Refresh in SmartApplets to update (manager mode)';
    }
    const backupDetail = document.getElementById('backup-count-detail');
    if (backupDetail && backupDetail.textContent.startsWith('Sign in')) {
      backupDetail.textContent = 'Saved on SD or internal storage';
    }
    const neoDetail = document.getElementById('neo-connection-detail');
    if (neoDetail && neoDetail.textContent.startsWith('Sign in')) {
      neoDetail.textContent = 'Connect the NEO with USB';
    }
    const neoConn = document.getElementById('neo-connection');
    if (neoConn && neoConn.textContent === 'Sign in') {
      neoConn.textContent = 'Waiting';
    }
  }
}

// Login UI
const loginBtn = document.getElementById('login-btn');
const loginDialog = document.getElementById('login-dialog');
const loginForm = document.getElementById('login-form');
const loginCancel = document.getElementById('login-cancel');

loginBtn.addEventListener('click', () => {
  if (isSessionAuthed()) {
    invalidateSession();
    return;
  }
  document.querySelector('#login-error').hidden = true;
  loginDialog.showModal();
  document.querySelector('#login-password').focus();
});
loginCancel.addEventListener('click', () => loginDialog.close());

loginForm.addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const pw = document.getElementById('login-password').value;
  try {
    const res = await fetch(API_BASE + '/login', {method:'POST', body: JSON.stringify({password: pw}), headers:{'Content-Type':'application/json'}});
    if (!res.ok) throw new Error('The password was not accepted.');
    const j = await res.json();
    setAuthToken(j.token);
    rememberTokenExpiry(j.expires_in);
    sessionVerified = true;
    loginDialog.close();
    loginForm.reset();
    updateSignInState();
    await refreshStatus();
    if (!IS_NEO_LINK_PAGE) loadCloudSyncConfig();
    if (IS_DASHBOARD) {
      await refreshFiles();
    }
    showNotice('Signed in.', 'success');
  } catch (error) {
    document.querySelector('#login-error').textContent = error.message === 'The password was not accepted.'
      ? error.message
      : 'The portal could not reach the device. Check the local connection and try again.';
    document.querySelector('#login-error').hidden = false;
  }
});

document.querySelector('#settings-close')?.addEventListener('click', () => settingsDialog.close());
document.querySelector('#settings-form')?.addEventListener('submit', (event) => {
  event.preventDefault();
  const settings = {
    deviceName: document.querySelector('#settings-device-name').value.trim() || 'Neo2 Buddy',
    sleep: document.querySelector('#settings-sleep').value,
    privateLive: document.querySelector('#settings-private-live').checked,
    preferSd: document.querySelector('#settings-prefer-sd').checked
  };
  localStorage.setItem('neo2_device_name', settings.deviceName);
  localStorage.setItem('neo2_portal_settings', JSON.stringify(settings));
  document.querySelector('#settings-device-title').textContent = settings.deviceName;
  const headerDeviceEl = document.getElementById('header-device-name');
  if (headerDeviceEl) headerDeviceEl.textContent = settings.deviceName;
  (async () => {
    if (!getAuthToken()) {
      document.querySelector('#settings-note').textContent = 'Saved locally. Sign in to apply settings to the buddy.';
      return;
    }
    try {
      const mode = settingsNetworkMode();
      const body = {
        device_name: settings.deviceName,
        sleep_timeout_seconds: Number(settings.sleep),
        keyboard_layout: document.querySelector('#settings-keyboard-layout').value,
        require_portal_auth: document.querySelector('#settings-require-auth').checked,
        auto_backup_on_connect: !!(document.querySelector('#settings-auto-backup') &&
          document.querySelector('#settings-auto-backup').checked),
        auto_cloud_sync_after_backup: !!(document.querySelector('#settings-auto-cloud') &&
          document.querySelector('#settings-auto-cloud').checked),
        neo_label: (document.querySelector('#settings-neo-label')?.value || '').trim(),
        network_mode: mode,
        hotspot_ssid: document.querySelector('#hotspot-ssid').value.trim(),
        wifi_dhcp: document.querySelector('#wifi-dhcp').checked,
        wifi_ip: document.querySelector('#wifi-ip').value.trim(),
        wifi_netmask: document.querySelector('#wifi-netmask').value.trim(),
        wifi_gateway: document.querySelector('#wifi-gateway').value.trim(),
        wifi_dns: document.querySelector('#wifi-dns').value.trim()
      };
      const hotspotPassword = document.querySelector('#hotspot-password').value;
      if (mode === 'direct') {
        if (!body.hotspot_ssid) {
          showNotice('Enter a hotspot name for Direct access.', 'error');
          return;
        }
        if (hotspotPassword && hotspotPassword.length < 8) {
          showNotice('Hotspot password must be at least 8 characters.', 'error');
          return;
        }
        if (hotspotPassword) body.hotspot_password = hotspotPassword;
        /* Keep / update saved STA networks even while staying on Direct access. */
        if (savedWifiNetworks.length) {
          const preferred = savedWifiNetworks.find((n) => n.preferred) || savedWifiNetworks[0];
          body.wifi_ssid = preferred.ssid;
          body.wifi_networks = savedWifiNetworks.map((n) => {
            const out = { ssid: n.ssid };
            if (n.password) out.password = n.password;
            return out;
          });
        }
      } else {
        let ssidSelect = document.querySelector('#wifi-ssid');
        let ssid = ssidSelect?.value.trim() || ssidSelect?.dataset.savedSsid || '';
        const preferred = savedWifiNetworks.find((n) => n.preferred);
        if (preferred) ssid = preferred.ssid;
        if (!ssid && savedWifiNetworks[0]) ssid = savedWifiNetworks[0].ssid;
        if (!ssid && !savedWifiNetworks.length) {
          showNotice('Add at least one home Wi‑Fi network (or scan and select one).', 'error');
          return;
        }
        if (ssid) {
          const wifiPassword = document.querySelector('#wifi-password').value;
          upsertSavedWifi(ssid, wifiPassword);
          body.wifi_ssid = ssid;
          if (wifiPassword) body.wifi_password = wifiPassword;
        }
        body.wifi_networks = savedWifiNetworks.map((n) => {
          const out = { ssid: n.ssid };
          if (n.password) out.password = n.password;
          return out;
        });
      }
      const res = await apiRequest('/settings', { method: 'POST', body: JSON.stringify(body), headers: { 'Content-Type': 'application/json' } });
      const result = await res.json().catch(() => ({}));
      if (result.rebooting) {
        const rebootMsg = mode === 'home'
          ? 'Network settings saved. Rejoin your home Wi‑Fi in about a minute, then open the portal at the buddy’s new IP.'
          : 'Network settings saved. Rejoin the buddy hotspot in about a minute, then open http://192.168.4.1/';
        showNotice('The buddy is restarting…', 'success');
        document.querySelector('#settings-note').textContent = rebootMsg;
      } else {
        showNotice('Device settings saved.', 'success');
        document.querySelector('#settings-note').textContent = 'Settings saved.';
      }
    } catch (e) {
      showNotice('Failed to save device settings: ' + (e.message || ''), 'error');
    }
  })();
});
document.querySelectorAll('input[name="settings-network-mode"]').forEach((input) => {
  input.addEventListener('change', syncSettingsNetworkModeUi);
});
document.querySelector('#settings-reset')?.addEventListener('click', () => {
  localStorage.removeItem('neo2_portal_settings');
  openSettings();
});

// Show or hide static IP inputs depending on DHCP toggle
function toggleStaticIpFields() {
  const dhcpEl = document.querySelector('#wifi-dhcp');
  const staticWrap = document.querySelector('#static-ip-fields');
  if (!dhcpEl || !staticWrap) return;
  if (dhcpEl.checked) {
    staticWrap.hidden = true;
    // disable inputs so they don't get submitted accidentally
    staticWrap.querySelectorAll('input').forEach(i => i.disabled = true);
  } else {
    staticWrap.hidden = false;
    staticWrap.querySelectorAll('input').forEach(i => i.disabled = false);
    // Warn the user that changing static IP may make the device inaccessible
    const note = document.querySelector('#settings-note');
    if (note) note.textContent = 'Warning: applying a static IP may make the device inaccessible. Proceed with caution.';
  }
}

// Toggle when the DHCP checkbox changes
document.querySelector('#wifi-dhcp')?.addEventListener('change', () => toggleStaticIpFields());

async function refreshStatus(){
  if (!isSessionAuthed()) {
    const conn = document.getElementById('connection');
    if (conn) conn.textContent = 'Not signed in';
    return;
  }
  try{
    // Prefer the consolidated /status path; fall back to legacy /usb/status.
    let res = await authFetch('/status').catch(() => authFetch('/usb/status'));
    if (!res.ok) {
      if (res.status === 401) {
        // authFetch already invalidated the session
        return;
      }
      const conn = document.getElementById('connection');
      if (conn) conn.textContent = 'Signed in · status unavailable';
      setNeoConnectionState({ usb_connected: false });
      statusUnchangedStreak = 0;
      return;
    }
    const j = await res.json();
    const fp = [
      j.usb_connected, j.usb_keyboard_active, j.usb_neo_ready, j.usb_flipping,
      j.ble_state, j.auto_backup_busy, j.auto_backup_on_connect,
      j.ip, j.battery_percent, j.charging, j.product || ''
    ].join('|');
    if (fp === statusFingerprint) statusUnchangedStreak++;
    else {
      statusUnchangedStreak = 0;
      statusFingerprint = fp;
    }
    sBleState = j.ble_state || 'idle';
    sBleConnected = sBleState === 'connected';
    setNeoConnectionState(j);
    applyFeatureFlags(j);
    const fmtBtn = document.getElementById('format-sd');
    if (fmtBtn) fmtBtn.hidden = !j.have_sdcard;
    const conn = document.getElementById('connection');
    if (conn) {
      const ble =
        j.ble_state === 'connected'
          ? 'Bluetooth connected'
          : j.ble_state === 'pairing'
            ? 'Bluetooth pairing'
            : 'Bluetooth idle';
      conn.textContent = j.ip ? `${j.ip} · ${ble}` : ble;
    }
    if (IS_DASHBOARD && sNeoKeyboardActive) {
      updateAppletCountFromCache();
    }
  }catch(e){
    if (!isSessionAuthed()) return;
    document.getElementById('connection').textContent = 'Signed in · reconnecting…';
    setNeoConnectionState({ usb_connected: false });
    statusUnchangedStreak = 0;
  }
}

function applyFeatureFlags(flags) {
  try {
    const haveSd = !!flags.have_sdcard;
    const sdPrefInput = document.querySelector('#settings-prefer-sd');
    if (sdPrefInput) {
      const label = sdPrefInput.closest('label');
      if (label) label.hidden = !haveSd;
    }
    const haveOled = !!flags.have_oled;
    const displayEl = document.querySelector('#settings-display') || document.querySelector('#display-controls');
    if (displayEl) displayEl.hidden = !haveOled;
    // Optionally show a note when features are disabled
    const notice = document.querySelector('#feature-notice') || document.createElement('div');
    notice.id = 'feature-notice';
    notice.className = 'feature-notice';
    notice.hidden = haveSd && haveOled;
    if (!haveSd || !haveOled) {
      notice.textContent = 'Some hardware features are disabled in this firmware build and are hidden from the UI.';
      document.querySelector('main').prepend(notice);
    }
  } catch (e) {
    // Non-fatal: keep portal functional even if feature flags can't be applied.
  }
}

function backupProgressDetail(status) {
  const phase = status?.auto_backup_phase || status?.phase || '';
  const cur = status?.auto_backup_current ?? status?.current ?? 0;
  const total = status?.auto_backup_total ?? status?.total ?? 8;
  if (phase === 'file' && cur > 0) {
    return `Backing up file ${cur}/${total}…`;
  }
  if (phase === 'settle') return 'Preparing backup…';
  if (phase === 'comms') return 'Switching Neo to manager mode…';
  if (phase === 'restart') return 'Returning Neo to keyboard mode…';
  return 'Backing up changed files, then returning Neo to keyboard mode…';
}

let sBackupBusy = false;
let sBackupPollTimer = null;
let sAwaitingBackupFinish = false;
/** True while Neo is in USB keyboard mode (live typing / emulation). */
let sNeoKeyboardActive = false;
let sNeoCommsReady = false;
let sBleState = 'idle';
let sBleConnected = false;
let sFetchedNeoSystemInfo = false;
let sAutoBackupOnConnect = false;
let sAutoBackupSaving = false;

const AUTO_BACKUP_TOGGLE_IDS = ['dashboard-auto-backup', 'backups-auto-backup', 'settings-auto-backup'];

function syncAutoBackupToggles(enabled, opts = {}) {
  sAutoBackupOnConnect = !!enabled;
  for (const id of AUTO_BACKUP_TOGGLE_IDS) {
    const el = document.getElementById(id);
    if (!el || el === opts.fromEl) continue;
    el.checked = sAutoBackupOnConnect;
  }
}

async function saveAutoBackupOnConnect(enabled, opts = {}) {
  if (!getAuthToken()) {
    showNotice('Sign in to change auto-backup.', 'error');
    syncAutoBackupToggles(!enabled);
    return;
  }
  if (sAutoBackupSaving) return;
  sAutoBackupSaving = true;
  const prev = sAutoBackupOnConnect;
  syncAutoBackupToggles(enabled, opts);
  try {
    const res = await apiRequest('/settings', {
      method: 'POST',
      body: JSON.stringify({ auto_backup_on_connect: !!enabled }),
      headers: { 'Content-Type': 'application/json' },
    });
    if (!res.ok) throw new Error('Could not save auto-backup setting.');
    if (!opts.quiet) {
      showNotice(
        enabled ? 'Auto-backup on connect enabled.' : 'Auto-backup on connect disabled.',
        'success'
      );
    }
    updatePortalNotices();
  } catch (error) {
    syncAutoBackupToggles(prev);
    showNotice(error.message || 'Could not save auto-backup setting.', 'error');
  } finally {
    sAutoBackupSaving = false;
  }
}

function bindAutoBackupToggle(id) {
  document.getElementById(id)?.addEventListener('change', (event) => {
    if (sAutoBackupSaving) return;
    saveAutoBackupOnConnect(event.target.checked, { fromEl: event.target });
  });
}

for (const id of ['dashboard-auto-backup', 'backups-auto-backup']) {
  bindAutoBackupToggle(id);
}

function bleAsmWarningLine() {
  if (!sBleConnected) return '';
  return '\n\nBluetooth is connected: switching to ASM (manager) mode may stop Neo keystrokes reaching your phone or PC until keyboard mode returns.';
}

/** Confirm leaving keyboard emulation for manager/file ops. Returns true to proceed. */
function confirmLeaveKeyboardMode(actionLabel) {
  if (!sNeoKeyboardActive || sBackupBusy) return true;
  return window.confirm(
    `${actionLabel} switches Neo out of keyboard mode into ASM (manager) mode.\n\n` +
      'Live typing pauses until Neo returns to keyboard mode (automatic after Backup now / Backup all).' +
      bleAsmWarningLine() +
      '\n\nContinue?'
  );
}

/**
 * Switch Neo to ASM/manager mode before Documents & Applets ops.
 * Neo defaults to keyboard/emulation; firmware ops also call ensure_comms, but the
 * portal must enter manager first so status/UI stay consistent.
 * @returns {Promise<boolean>} true to proceed with the caller’s action
 */
async function ensureManagerMode(actionLabel, opts = {}) {
  const needConfirm = opts.confirm !== false;
  if (sBackupBusy) {
    showNotice('Wait for the backup to finish first.', 'error');
    return false;
  }
  if (needConfirm && !confirmLeaveKeyboardMode(actionLabel)) return false;
  if (sNeoCommsReady) return true;
  try {
    await apiRequest('/neo/manager', { method: 'POST' });
    await refreshStatus();
    if (!sNeoCommsReady) {
      showNotice('Neo did not enter manager mode. Check the USB connection and try again.', 'error');
      return false;
    }
    return true;
  } catch (error) {
    showNotice(`Could not switch to manager mode: ${error.message || error}`, 'error');
    return false;
  }
}

function updatePortalNotices() {
  const el = document.getElementById('portal-notices');
  if (!el) return;
  if (!getAuthToken()) {
    el.innerHTML = '';
    el.hidden = true;
    return;
  }
  let html = '';
  if (sBleConnected) {
    const docsHint = IS_DASHBOARD
      ? ' Use Scan, Backup, or Refresh only when you intend to leave keyboard mode.'
      : ' Open <a href="index.html">Documents</a> for file ops — those switch the Neo to ASM mode.';
    html =
      `<p class="portal-notice portal-notice-ble" role="status">` +
      `<strong>Bluetooth keyboard connected.</strong> ASM (manager) mode may <strong>stop Neo keystrokes</strong> on your paired device until keyboard mode returns.${docsHint}</p>`;
  } else if (IS_DASHBOARD && sNeoKeyboardActive) {
    html =
      '<p class="portal-notice portal-notice-info" role="status">' +
      'Neo is in <strong>keyboard mode</strong>. This page does not contact the Neo until you choose Scan, Backup, or Refresh.</p>';
  } else if (IS_TYPING_PAGE && sNeoKeyboardActive) {
    html =
      '<p class="portal-notice portal-notice-info" role="status">' +
      'Keyboard mode — live typing and Bluetooth passthrough only. No background polling here.</p>';
  }
  el.innerHTML = html;
  el.hidden = !html;
}

function setNeoConnectionState(status) {
  if (!getAuthToken()) return;
  const connected = !!status?.usb_connected;
  const keyboardActive = !!status?.usb_keyboard_active;
  const commsReady = !!status?.usb_neo_ready;
  const wasCommsReady = sNeoCommsReady;
  sNeoKeyboardActive = !!(keyboardActive && !commsReady);
  sNeoCommsReady = commsReady;
  const product = status?.product || '';
  const busDevs = status?.usb_bus_devices ?? null;
  const hostActive = status?.usb_host_active;
  const flipping = status?.usb_flipping;
  const portHint = status?.usb_port_hint || '';
  const bakBusy = !!status?.auto_backup_busy;
  const bakEnabled = !!status?.auto_backup_on_connect;
  syncAutoBackupToggles(bakEnabled, { quiet: true });

  if (bakBusy) {
    document.querySelector('#neo-connection').textContent = 'Backing up…';
  } else {
    document.querySelector('#neo-connection').textContent = connected
      ? (keyboardActive && !commsReady ? 'Keyboard' : 'Ready')
      : (flipping ? 'Switching…' : 'Waiting');
  }
  let detail = connected
    ? (keyboardActive && !commsReady
      ? 'Keyboard mode — file/backup actions switch to ASM only when you click them'
      : 'ASM (manager) mode — USB protocol active')
    : 'Connect the NEO with USB';
  if (bakBusy) {
    detail = backupProgressDetail(status);
  } else if (!connected && hostActive && busDevs === 0) {
    detail = portHint || 'Neo USB-B needs 5V (powerbank OK). Internal AAs alone are not enough';
  } else if (!connected && hostActive && busDevs > 0) {
    detail = `USB device detected (${busDevs}), negotiating Neo protocol…`;
  } else if (connected && bakEnabled && keyboardActive && !commsReady) {
    detail = 'Keyboard mode · auto-backup on connect switches to ASM briefly';
    if (sBleConnected) detail += ' (Bluetooth keystrokes pause during backup)';
  }
  document.querySelector('#neo-connection-detail').textContent = detail;
  document.querySelector('#neo-model').textContent = connected || bakBusy ? product || 'NEO' : '--';
  const modelDetail = document.querySelector('#neo-model-detail');
  if (modelDetail) {
    if (!connected && !bakBusy) {
      modelDetail.textContent = 'Reported by the NEO when connected';
    } else if (sNeoKeyboardActive) {
      modelDetail.textContent = `${product || 'Neo2'} · keyboard mode`;
    } else if (modelDetail.textContent === 'Reported by the NEO when connected' ||
        modelDetail.textContent === 'Loading device info…' ||
        modelDetail.textContent.endsWith('· keyboard mode')) {
      modelDetail.textContent = 'Loading device info…';
    }
  }
  if (bakBusy) {
    sAwaitingBackupFinish = true;
    setBackupBusyUi(true);
  } else if (!sBackupPollTimer) {
    setBackupBusyUi(false);
  }
  if (connected && !bakBusy) {
    if (commsReady && !wasCommsReady) {
      sFetchedNeoSystemInfo = false;
      refreshNeoSystemInfo();
    } else if (commsReady && !sFetchedNeoSystemInfo) {
      refreshNeoSystemInfo();
    }
  }
  if (!commsReady) {
    sFetchedNeoSystemInfo = false;
  }
  if (IS_DASHBOARD && sNeoKeyboardActive) {
    updateAppletCountFromCache();
  }
  updatePortalNotices();
}

function setBackupBusyUi(busy, opts = {}) {
  const wasBusy = sBackupBusy;
  sBackupBusy = !!busy;
  const ids = ['neo-backup-now', 'neo-read-all', 'refresh-neo-files', 'neo-manager', 'neo-rescan'];
  for (const id of ids) {
    const el = document.getElementById(id);
    if (el) el.disabled = sBackupBusy;
  }
  if (sBackupBusy) {
    if (!sBackupPollTimer) {
      sBackupPollTimer = setInterval(pollBackupBusy, 1500);
    }
  } else if (sBackupPollTimer && !opts.keepPolling) {
    clearInterval(sBackupPollTimer);
    sBackupPollTimer = null;
  }
  if (wasBusy && !sBackupBusy && sAwaitingBackupFinish) {
    sAwaitingBackupFinish = false;
    if (opts.fromPoll) {
      if (opts.lastResult && opts.lastResult !== 'ESP_OK') {
        showNotice(`Backup finished with: ${opts.lastResult}`, 'error');
      } else {
        showNotice('Backup finished. Neo returned to keyboard mode.', 'success');
      }
      refreshFiles();
      refreshStatus();
    }
  }
}

async function pollBackupBusy() {
  if (!getAuthToken()) {
    if (sBackupPollTimer) {
      clearInterval(sBackupPollTimer);
      sBackupPollTimer = null;
    }
    return;
  }
  try {
    const res = await authFetch('/neo/autobackup');
    if (!res.ok) return;
    const j = await res.json();
    if (j.busy) {
      setBackupBusyUi(true);
      const detail = document.querySelector('#neo-connection-detail');
      if (detail) detail.textContent = backupProgressDetail(j);
      const notice = document.querySelector('#neo-file-notice');
      if (notice && j.phase === 'file' && j.current > 0) {
        notice.hidden = false;
        notice.dataset.type = 'info';
        notice.textContent = `Backing up file ${j.current}/${j.total || 8}…`;
      }
      return;
    }
    setBackupBusyUi(false, { fromPoll: true, lastResult: j.last_result });
  } catch (e) {
    // keep polling while status refresh may recover
  }
}

async function refreshNeoSystemInfo() {
  if (!getAuthToken()) return;
  if (sNeoKeyboardActive) return;
  try {
    const [infoRes, modeRes] = await Promise.all([
      authFetch('/command/info'),
      authFetch('/command/mode')
    ]);
    if (!infoRes.ok) return;
    const info = await infoRes.json();
    const mode = modeRes.ok ? (await modeRes.json()).mode : info.mode;
    const detail = document.querySelector('#neo-model-detail');
    if (detail) {
      detail.textContent = `${info.version || 'Neo2'} · ${mode || 'unknown'} · ${formatBytes(info.free_rom || 0)} ROM free · ${formatBytes(info.free_ram || 0)} RAM free`;
    }
    sFetchedNeoSystemInfo = true;
  } catch (error) {
    // Non-fatal when the device is switching modes.
  }
}

async function rescanNeo() {
  const btn = document.getElementById('neo-rescan');
  const detail = document.querySelector('#neo-connection-detail');
  setButtonBusy(btn, true, 'Scanning…');
  if (detail) detail.textContent = 'Scanning OTG1 and triggering Neo handshake…';
  try {
    const res = await authFetch('/neo/rescan', { method: 'POST' });
    const j = await res.json().catch(() => ({}));
    if (!res.ok || !j.ok) {
      if (detail) detail.textContent = j.error ? `Scan failed: ${j.error}` : 'Scan failed';
      return;
    }
    if (j.neo_ready) {
      setNeoConnectionState({ usb_connected: true, product: 'AlphaSmart Neo2', usb_bus_devices: j.bus_devices, usb_neo_ready: true });
    } else if (j.flipping) {
      setNeoConnectionState({ usb_connected: false, usb_flipping: true, usb_bus_devices: j.bus_devices, usb_host_active: true });
    } else if (j.bus_devices > 0) {
      const first = j.devices?.[0];
      const ids = first ? ` (VID 0x${first.vid.toString(16)} PID 0x${first.pid.toString(16)})` : '';
      if (detail) detail.textContent = `Found ${j.bus_devices} USB device(s)${ids} — flip in progress or not a Neo`;
    } else {
      if (detail) detail.textContent = 'No USB device — apply 5V to Neo USB-B, check OTG1 data cable, then Connect Neo';
    }
    refreshStatus();
  } catch (e) {
    if (detail) detail.textContent = 'Scan request failed';
  } finally {
    setButtonBusy(btn, false);
  }
}

document.getElementById('neo-rescan')?.addEventListener('click', () => rescanNeo());

async function refreshFiles() {
  if (!filesEmpty || !fileList) return;
  const button = document.querySelector('#refresh-files');
  setButtonBusy(button, true, 'Refreshing…');
  try {
    const response = await authFetch('/files');
    if (!response.ok) return;
    const files = await response.json();
    filesEmpty.hidden = files.length > 0;
    fileList.hidden = files.length === 0;
    fileList.innerHTML = files.map((file) => renderBackupFile(file)).join('');
    const countEl = document.querySelector('#backup-count');
    const detailEl = document.querySelector('#backup-count-detail');
    if (countEl) countEl.textContent = String(files.length);
    if (detailEl) detailEl.textContent = files.length === 1 ? '1 saved NEO document' : 'Saved NEO documents';
  } catch (error) {
    const countEl = document.querySelector('#backup-count');
    const detailEl = document.querySelector('#backup-count-detail');
    if (countEl) countEl.textContent = '--';
    if (detailEl) detailEl.textContent = 'Saved on SD or internal storage';
  } finally {
    setButtonBusy(button, false);
  }
}

function updateAppletCountFromCache() {
  const countEl = document.querySelector('#applet-count');
  const detailEl = document.querySelector('#applet-count-detail');
  if (!countEl || !detailEl) return;
  const cached = sessionStorage.getItem('neo_applet_count');
  if (cached === null || cached === '') return;
  const n = Number(cached);
  if (!Number.isFinite(n)) return;
  countEl.textContent = cached;
  detailEl.textContent = n === 1
    ? '1 SmartApplet (cached — use Refresh for an up-to-date list)'
    : `${n} SmartApplets (cached — use Refresh for an up-to-date list)`;
}

async function refreshApplets(opts = {}) {
  if (!(await ensureManagerMode('Refreshing applets', { confirm: !!opts.confirm }))) return;
  const button = document.querySelector('#refresh-applets');
  setButtonBusy(button, true, 'Refreshing…');
  try {
    const response = await authFetch('/command/list_applets');
    if (!response.ok) return;
    const applets = await response.json();
    if (appletsEmpty) appletsEmpty.hidden = applets.length > 0;
    if (appletList) {
      appletList.hidden = applets.length === 0;
      appletList.innerHTML = applets.map((applet) => renderApplet(applet)).join('');
    }
    rememberInstalledApplets(applets);
    if (document.getElementById('stock-store-dialog')?.open) {
      applyStockStoreFilter();
    }
    sessionStorage.setItem('neo_applet_count', String(applets.length));
    const countEl = document.querySelector('#applet-count');
    const detailEl = document.querySelector('#applet-count-detail');
    if (countEl) countEl.textContent = String(applets.length);
    if (detailEl) {
      detailEl.textContent = applets.length === 1 ? '1 installed SmartApplet' : 'Installed SmartApplets';
    }
  } catch (error) {
    const countEl = document.querySelector('#applet-count');
    const detailEl = document.querySelector('#applet-count-detail');
    if (countEl) countEl.textContent = '--';
    if (detailEl) detailEl.textContent = 'Connect the NEO to inspect applets';
  } finally {
    setButtonBusy(button, false);
  }
}

const STOCK_CATEGORY_LABELS = {
  game: 'Game',
  organize: 'Organize',
  write: 'Write',
  focus: 'Focus',
  learn: 'Learn',
  app: 'App',
};

const STOCK_CATEGORY_BADGES = {
  game: 'GAME',
  organize: 'ORG',
  write: 'WRITE',
  focus: 'FOCUS',
  learn: 'LEARN',
  app: 'APP',
};

let stockStoreFilter = 'all';
let stockStoreQuery = '';
let stockStoreCache = [];
let stockStorePendingInstall = null;
let stockStoreInstallBusy = false;
/** Applet IDs currently on the Neo (from last Refresh / install). */
let installedAppletIds = new Set();

function rememberInstalledApplets(applets) {
  installedAppletIds = new Set(
    (Array.isArray(applets) ? applets : [])
      .map((a) => Number(a?.id ?? a?.applet_id))
      .filter((id) => Number.isFinite(id) && id > 0)
  );
}

function isStockAppletInstalled(app) {
  const id = Number(app?.applet_id);
  return Number.isFinite(id) && installedAppletIds.has(id);
}

function stockMatchesQuery(app, query) {
  if (!query) return true;
  const hay = [
    app.name,
    app.slug,
    app.summary,
    app.blurb,
    app.how_to,
    app.category,
    stockCategoryLabel(app.category || 'app'),
    app.applet_id != null ? `0x${Number(app.applet_id).toString(16)}` : '',
    app.applet_id != null ? String(app.applet_id) : '',
  ]
    .filter(Boolean)
    .join(' ')
    .toLowerCase();
  return query.split(/\s+/).every((token) => token && hay.includes(token));
}

function stockCategoryLabel(cat) {
  return STOCK_CATEGORY_LABELS[cat] || (cat ? cat.charAt(0).toUpperCase() + cat.slice(1) : 'App');
}

function stockCategoryBadge(cat) {
  return STOCK_CATEGORY_BADGES[cat] || stockCategoryLabel(cat).slice(0, 4).toUpperCase();
}

function renderStockApplet(app) {
  const ver = `${app.version_major}.${app.version_minor}${app.version_rev || ''}`;
  const bytes = Number(app.bytes || 0);
  const bundled = app.bundled !== false && bytes > 0;
  const cat = app.category || 'app';
  const catLabel = stockCategoryLabel(cat);
  const summary = app.summary || app.blurb || '';
  const howTo = app.how_to || '';
  const installed = isStockAppletInstalled(app);
  const installBtn = !bundled
    ? `<button class="table-action" type="button" disabled title="Rebuild firmware with tools/build-stock-applets.ps1">Not bundled</button>`
    : installed
      ? `<button class="table-action" type="button" data-stock-action="install" data-stock-slug="${escapeHtml(app.slug)}" title="Replace the build already on the Neo">Reinstall</button>`
      : `<button class="table-action primary-action" type="button" data-stock-action="install" data-stock-slug="${escapeHtml(app.slug)}">Install on Neo</button>`;
  const deckBtn = app.slug === 'flash-cards' && bundled
    ? `<button class="table-action" type="button" data-stock-action="edit-decks" data-stock-slug="flash-cards" title="Open the Flash Cards deck library">Edit decks</button>`
    : '';
  const installedBadge = installed
    ? `<span class="store-card-installed" title="This applet ID is already on the connected Neo">Installed</span>`
    : '';
  return `<article class="store-card${installed ? ' is-installed' : ''}" data-category="${escapeHtml(cat)}" data-stock-slug="${escapeHtml(app.slug)}">
  <div class="store-card-top">
    <span class="store-card-icon cat-${escapeHtml(cat)}" aria-hidden="true">${escapeHtml(stockCategoryBadge(cat))}</span>
    <div>
      <h3 class="store-card-title">${escapeHtml(app.name || app.slug)}${installedBadge}</h3>
      <p class="store-card-meta">${escapeHtml(catLabel)} · ID 0x${Number(app.applet_id).toString(16).toUpperCase()} · v${escapeHtml(ver)} · ${formatBytes(bytes)}</p>
    </div>
  </div>
  <p class="store-card-summary">${escapeHtml(summary)}</p>
  ${howTo ? `<p class="store-card-howto"><strong>On the Neo:</strong> ${escapeHtml(howTo)}</p>` : ''}
  <div class="store-card-actions applet-actions">${deckBtn}${installBtn}</div>
</article>`;
}

function renderStockFilters(applets) {
  const filters = document.querySelector('#stock-store-filters');
  const toolbar = document.querySelector('#stock-store-toolbar');
  if (!filters || !toolbar) return;
  const cats = [];
  applets.forEach((a) => {
    const c = a.category || 'app';
    if (!cats.includes(c)) cats.push(c);
  });
  const chips = [`<button type="button" class="store-filter${stockStoreFilter === 'all' ? ' is-active' : ''}" data-store-filter="all">All</button>`]
    .concat(cats.map((c) => {
      const active = stockStoreFilter === c ? ' is-active' : '';
      return `<button type="button" class="store-filter${active}" data-store-filter="${escapeHtml(c)}">${escapeHtml(stockCategoryLabel(c))}</button>`;
    }));
  filters.innerHTML = chips.join('');
  toolbar.hidden = applets.length === 0;
}

function applyStockStoreFilter() {
  const list = document.querySelector('#stock-applet-list');
  const empty = document.querySelector('#stock-applets-empty');
  const countEl = document.querySelector('#stock-store-count');
  const filters = document.querySelectorAll('#stock-store-filters .store-filter');
  filters.forEach((btn) => {
    btn.classList.toggle('is-active', btn.dataset.storeFilter === stockStoreFilter);
  });
  const query = stockStoreQuery.trim().toLowerCase();
  let filtered = stockStoreFilter === 'all'
    ? stockStoreCache
    : stockStoreCache.filter((a) => (a.category || 'app') === stockStoreFilter);
  filtered = filtered.filter((a) => stockMatchesQuery(a, query));
  if (list) {
    list.innerHTML = filtered.map(renderStockApplet).join('');
    list.hidden = filtered.length === 0;
  }
  if (empty && stockStoreCache.length) {
    empty.hidden = filtered.length > 0;
    if (!filtered.length) {
      empty.querySelector('h3').textContent = query ? 'No matching applets' : 'No applets in this category';
      empty.querySelector('p').textContent = query
        ? 'Try another search term or clear the search box.'
        : 'Pick another category or search the full catalog.';
    }
  }
  if (countEl) {
    if (!stockStoreCache.length) {
      countEl.textContent = '';
    } else if (query || stockStoreFilter !== 'all') {
      countEl.textContent = `${filtered.length} of ${stockStoreCache.length}`;
    } else {
      countEl.textContent = `${stockStoreCache.length} applet${stockStoreCache.length === 1 ? '' : 's'} in firmware`;
    }
  }
}

/** Normalize Anki Notes TXT / CSV / | or tab decks into front|back lines (max 16 cards for Neo). */
function normalizeFlashDeck(text) {
  const lines = String(text || '')
    .replace(/^\uFEFF/, '')
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l && !l.startsWith('#') && !l.startsWith('tags:'));
  const out = [];
  for (const line of lines) {
    let front = '';
    let back = '';
    if (line.includes('|')) {
      const i = line.indexOf('|');
      front = line.slice(0, i).trim();
      back = line.slice(i + 1).trim();
    } else if (line.includes('\t')) {
      const parts = line.split('\t');
      front = (parts[0] || '').trim();
      back = (parts[1] || '').trim();
    } else if (line.includes(',')) {
      // Simple CSV: "front","back" or front,back
      const m = line.match(/^"([^"]*)"\s*,\s*"([^"]*)"/) || line.match(/^([^,]+)\s*,\s*(.+)$/);
      if (m) {
        front = m[1].trim();
        back = m[2].trim().replace(/^"|"$/g, '');
      }
    }
    // Strip simple HTML from Anki exports
    front = front.replace(/<[^>]+>/g, ' ').replace(/\s+/g, ' ').trim();
    back = back.replace(/<[^>]+>/g, ' ').replace(/\s+/g, ' ').trim();
    if (front && back) {
      out.push(`${front.slice(0, 23)}|${back.slice(0, 23)}`);
    }
    if (out.length >= 16) break;
  }
  return out.join('\n') + (out.length ? '\n' : '');
}

const FLASH_PROTECTED_ID = 'en-nl-basic';
let flashDeckList = [];
let flashDeckActiveId = null;
let flashDeckDraft = null;

function flashDeckIdFromName(name) {
  const raw = String(name || 'deck')
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '')
    .slice(0, 22);
  return raw || 'deck';
}

function cardsFromPipeText(text) {
  return String(text || '')
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => {
      const i = line.indexOf('|');
      if (i < 0) return null;
      const front = line.slice(0, i).trim().slice(0, 23);
      const back = line.slice(i + 1).trim().slice(0, 23);
      if (!front || !back) return null;
      return { front, back };
    })
    .filter(Boolean)
    .slice(0, 16);
}

function collectFlashCardsFromDom(opts = {}) {
  /* keepIncomplete: preserve blank/partial rows while editing.
     Save/push strip those so the Neo only gets complete cards. */
  const keepIncomplete = opts.keepIncomplete === true;
  const rows = document.querySelectorAll('#flashdeck-cards .flashdeck-card-row');
  const cards = [];
  rows.forEach((row) => {
    const front = row.querySelector('[data-flash-side="front"]')?.value?.trim().slice(0, 23) || '';
    const back = row.querySelector('[data-flash-side="back"]')?.value?.trim().slice(0, 23) || '';
    if (keepIncomplete || (front && back)) {
      cards.push({ front, back });
    }
  });
  return cards;
}

function completeFlashCards(cards) {
  return (Array.isArray(cards) ? cards : [])
    .map((c) => ({
      front: String(c?.front || '').trim().slice(0, 23),
      back: String(c?.back || '').trim().slice(0, 23),
    }))
    .filter((c) => c.front && c.back);
}

function syncFlashDraftFromDom() {
  if (!flashDeckDraft) return;
  const nameEl = document.querySelector('#flashdeck-name');
  flashDeckDraft.name = (nameEl?.value || '').trim().slice(0, 40) || 'Untitled deck';
  flashDeckDraft.cards = collectFlashCardsFromDom({ keepIncomplete: true });
}

function renderFlashSetList() {
  const list = document.querySelector('#flashdeck-set-list');
  if (!list) return;
  if (!flashDeckList.length) {
    list.innerHTML = '<p class="flashdeck-set-meta">No sets yet.</p>';
    return;
  }
  list.innerHTML = flashDeckList.map((d) => {
    const active = d.id === flashDeckActiveId ? ' is-active' : '';
    const n = Number(d.cards || 0);
    return `<button type="button" class="flashdeck-set${active}" data-flash-id="${escapeHtml(d.id)}" role="listitem">
      <span class="flashdeck-set-name">${escapeHtml(d.name || d.id)}</span>
      <span class="flashdeck-set-meta">${n} card${n === 1 ? '' : 's'}</span>
    </button>`;
  }).join('');
}

function renderFlashEditor() {
  const layout = document.querySelector('#flashdeck-layout');
  const empty = document.querySelector('#flashdeck-empty');
  const meta = document.querySelector('#flashdeck-meta');
  const nameEl = document.querySelector('#flashdeck-name');
  const cardsEl = document.querySelector('#flashdeck-cards');
  const delBtn = document.querySelector('#flashdeck-delete');
  if (!layout || !cardsEl) return;

  if (!getAuthToken()) {
    layout.hidden = true;
    if (empty) {
      empty.hidden = false;
      empty.querySelector('h3').textContent = 'Sign in to edit decks';
      empty.querySelector('p').textContent = 'Your flashcard sets appear here after you sign in.';
    }
    return;
  }

  if (!flashDeckDraft) {
    layout.hidden = true;
    if (empty) {
      empty.hidden = false;
      empty.querySelector('h3').textContent = 'No deck selected';
      empty.querySelector('p').textContent = 'Pick a set on the left, or create a new one.';
    }
    return;
  }

  if (empty) empty.hidden = true;
  layout.hidden = false;
  if (nameEl) nameEl.value = flashDeckDraft.name || '';
  const n = flashDeckDraft.cards?.length || 0;
  if (meta) {
    meta.textContent = `ID ${flashDeckDraft.id} · ${n}/16 cards · sides ≤ 23 chars`;
  }
  if (delBtn) {
    delBtn.disabled = flashDeckDraft.id === FLASH_PROTECTED_ID;
    delBtn.title = flashDeckDraft.id === FLASH_PROTECTED_ID
      ? 'The English→Dutch starter set cannot be deleted'
      : 'Delete this set from the buddy';
  }
  const cards = Array.isArray(flashDeckDraft.cards) ? flashDeckDraft.cards : [];
  const rows = cards.length ? cards : [{ front: '', back: '' }];
  cardsEl.innerHTML = rows.map((c, i) => `<div class="flashdeck-card-row" data-card-index="${i}">
    <input type="text" maxlength="23" data-flash-side="front" placeholder="Front" value="${escapeHtml(c.front || '')}" autocomplete="off">
    <input type="text" maxlength="23" data-flash-side="back" placeholder="Back" value="${escapeHtml(c.back || '')}" autocomplete="off">
    <button class="table-action" type="button" data-flash-remove-card title="Remove card">Remove</button>
  </div>`).join('');
}

async function loadFlashDeck(id) {
  if (!id) return;
  syncFlashDraftFromDom();
  try {
    const response = await apiRequest(`/flashdecks/${encodeURIComponent(id)}`);
    const data = await response.json();
    flashDeckActiveId = id;
    flashDeckDraft = {
      id,
      name: data.name || id,
      cards: Array.isArray(data.cards) ? data.cards.map((c) => ({
        front: String(c.front || '').slice(0, 23),
        back: String(c.back || '').slice(0, 23),
      })) : [],
    };
    renderFlashSetList();
    renderFlashEditor();
  } catch (error) {
    showNotice(error.message || 'Could not load deck', 'error');
  }
}

async function refreshFlashDecks(opts = {}) {
  const button = document.querySelector('#flashdeck-refresh');
  const empty = document.querySelector('#flashdeck-empty');
  const layout = document.querySelector('#flashdeck-layout');
  if (!getAuthToken()) {
    flashDeckList = [];
    flashDeckActiveId = null;
    flashDeckDraft = null;
    renderFlashSetList();
    renderFlashEditor();
    return;
  }
  setButtonBusy(button, true, 'Loading…');
  try {
    const response = await apiRequest('/flashdecks');
    const data = await response.json();
    flashDeckList = Array.isArray(data.decks) ? data.decks : [];
    if (!flashDeckActiveId || !flashDeckList.some((d) => d.id === flashDeckActiveId)) {
      flashDeckActiveId = flashDeckList[0]?.id || null;
    }
    if (flashDeckActiveId && opts.keepDraft !== true) {
      await loadFlashDeck(flashDeckActiveId);
    } else {
      renderFlashSetList();
      renderFlashEditor();
    }
    if (!flashDeckList.length) {
      if (layout) layout.hidden = true;
      if (empty) {
        empty.hidden = false;
        empty.querySelector('h3').textContent = 'No decks yet';
        empty.querySelector('p').textContent = 'Create a new set or import a file.';
      }
    }
  } catch (error) {
    flashDeckList = [];
    flashDeckDraft = null;
    if (layout) layout.hidden = true;
    if (empty) {
      empty.hidden = false;
      empty.querySelector('h3').textContent = 'Deck library unavailable';
      empty.querySelector('p').textContent = error.message || 'Sign in and try again.';
    }
  } finally {
    setButtonBusy(button, false);
  }
}

async function saveFlashDeck() {
  if (!flashDeckDraft?.id) return;
  syncFlashDraftFromDom();
  const cards = completeFlashCards(flashDeckDraft.cards);
  if (!cards.length) {
    showNotice('Add at least one card with both sides filled.', 'warning');
    return;
  }
  try {
    showNotice('Saving deck…', 'info');
    await apiRequest(`/flashdecks/${encodeURIComponent(flashDeckDraft.id)}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name: flashDeckDraft.name, cards }),
    });
    flashDeckDraft.cards = cards;
    showNotice(`Saved “${flashDeckDraft.name}” (${cards.length} cards).`, 'success');
    await refreshFlashDecks({ keepDraft: false });
  } catch (error) {
    showNotice(error.message || 'Save failed', 'error');
  }
}

async function pushFlashDeck() {
  if (!flashDeckDraft?.id) return;
  syncFlashDraftFromDom();
  const cards = completeFlashCards(flashDeckDraft.cards);
  if (!cards.length) {
    showNotice('Save at least one card before pushing.', 'warning');
    return;
  }
  if (!window.confirm(`Save and push “${flashDeckDraft.name}” to Flash Cards on the Neo?\n\nThis replaces the active Neo deck (max 16 cards).`)) {
    return;
  }
  if (!(await ensureManagerMode('Pushing a Flash Cards deck', { confirm: false }))) return;
  try {
    await apiRequest(`/flashdecks/${encodeURIComponent(flashDeckDraft.id)}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name: flashDeckDraft.name, cards }),
    });
    flashDeckDraft.cards = cards;
    showNotice('Pushing deck to Neo…', 'info');
    await apiRequest(`/flashdecks/${encodeURIComponent(flashDeckDraft.id)}/push`, { method: 'POST' });
    showNotice(`Pushed “${flashDeckDraft.name}”. Open Flash Cards on the Neo.`, 'success');
    await refreshFlashDecks();
  } catch (error) {
    const msg = String(error.message || error);
    if (msg.includes('neo_not_connected') || msg.toLowerCase().includes('neo not connected')) {
      showNotice('Connect the Neo by USB, then push again.', 'warning');
    } else {
      showNotice(msg || 'Push failed', 'error');
    }
  }
}

async function deleteFlashDeck() {
  if (!flashDeckDraft?.id || flashDeckDraft.id === FLASH_PROTECTED_ID) return;
  if (!window.confirm(`Delete set “${flashDeckDraft.name}” from the buddy?`)) return;
  try {
    await apiRequest(`/flashdecks/${encodeURIComponent(flashDeckDraft.id)}`, { method: 'DELETE' });
    showNotice('Set deleted.', 'success');
    flashDeckActiveId = null;
    flashDeckDraft = null;
    await refreshFlashDecks();
  } catch (error) {
    showNotice(error.message || 'Delete failed', 'error');
  }
}

function newFlashDeck() {
  const name = window.prompt('Name for the new set:', 'My cards');
  if (name == null) return;
  const trimmed = name.trim().slice(0, 40) || 'Untitled deck';
  let id = flashDeckIdFromName(trimmed);
  if (flashDeckList.some((d) => d.id === id) || id === FLASH_PROTECTED_ID) {
    id = `${id}-${Date.now().toString(36).slice(-4)}`.slice(0, 23);
  }
  flashDeckActiveId = id;
  flashDeckDraft = { id, name: trimmed, cards: [{ front: '', back: '' }] };
  renderFlashSetList();
  renderFlashEditor();
}

async function importFlashDeckToLibrary(file) {
  if (!file) return;
  try {
    const text = await file.text();
    const cards = cardsFromPipeText(normalizeFlashDeck(text));
    if (!cards.length) {
      showNotice('No cards found. Export Anki as Notes plain text, or use front|back / CSV lines.', 'warning');
      return;
    }
    const base = file.name.replace(/\.[^.]+$/, '') || 'imported';
    const name = base.slice(0, 40);
    let id = flashDeckIdFromName(base);
    if (flashDeckList.some((d) => d.id === id) || id === FLASH_PROTECTED_ID) {
      id = `${id}-${Date.now().toString(36).slice(-4)}`.slice(0, 23);
    }
    showNotice(`Importing ${cards.length} cards…`, 'info');
    await apiRequest(`/flashdecks/${encodeURIComponent(id)}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name, cards }),
    });
    flashDeckActiveId = id;
    showNotice(`Imported “${name}”. Push to Neo when ready.`, 'success');
    await refreshFlashDecks();
  } catch (error) {
    showNotice(error.message || 'Import failed', 'error');
  }
}

async function refreshStockApplets() {
  const button = document.querySelector('#refresh-stock-applets');
  const empty = document.querySelector('#stock-applets-empty');
  const list = document.querySelector('#stock-applet-list');
  const toolbar = document.querySelector('#stock-store-toolbar');
  setButtonBusy(button, true, 'Loading…');
  try {
    const response = await apiRequest('/neo/stock-applets');
    if (!response.ok) throw new Error('Could not load Applet Store catalog');
    const data = await response.json();
    const applets = Array.isArray(data.applets) ? data.applets : [];
    stockStoreCache = applets;
    if (stockStoreFilter !== 'all' && !applets.some((a) => (a.category || 'app') === stockStoreFilter)) {
      stockStoreFilter = 'all';
    }
    if (empty) {
      empty.hidden = applets.length > 0;
      if (!applets.length) {
        const paused = data.installs_enabled === false;
        empty.querySelector('h3').textContent = paused ? 'Store paused' : 'No stock applets';
        empty.querySelector('p').textContent = paused
          ? (data.note || 'Applet Store installs are paused while stock applets are retested.')
          : 'Rebuild firmware after running tools/build-stock-applets.ps1.';
      }
    }
    renderStockFilters(applets);
    if (list) {
      list.hidden = applets.length === 0;
    }
    applyStockStoreFilter();
  } catch (error) {
    stockStoreCache = [];
    if (toolbar) toolbar.hidden = true;
    if (empty) {
      empty.hidden = false;
      empty.querySelector('h3').textContent = 'Applet Store unavailable';
      empty.querySelector('p').textContent = error.message || 'Sign in and try again.';
    }
    if (list) {
      list.hidden = true;
      list.innerHTML = '';
    }
  } finally {
    setButtonBusy(button, false);
  }
}

function setStockStoreStatus(message, type = 'info') {
  const storeStatus = document.getElementById('stock-store-status');
  if (!storeStatus) {
    if (message) showNotice(message, type);
    return;
  }
  storeStatus.textContent = message || '';
  storeStatus.dataset.type = type;
  storeStatus.hidden = !message;
  if (message) {
    queueMicrotask(() => {
      try {
        storeStatus.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
      } catch (_) { /* ignore */ }
    });
  }
}

function hideStockStoreConfirm() {
  stockStorePendingInstall = null;
  const panel = document.getElementById('stock-store-confirm');
  if (panel) panel.hidden = true;
  const copy = document.getElementById('stock-store-confirm-copy');
  if (copy) copy.textContent = '';
}

function promptStockStoreInstall(slug, name) {
  if (!slug || stockStoreInstallBusy) return;
  const app = stockStoreCache.find((a) => a.slug === slug);
  const detail = app?.summary || app?.blurb || '';
  const how = app?.how_to ? `\n\nOn the Neo: ${app.how_to}` : '';
  const reinstall = isStockAppletInstalled(app);
  stockStorePendingInstall = { slug, name: name || slug };
  setStockStoreStatus('', 'info');
  const panel = document.getElementById('stock-store-confirm');
  const copy = document.getElementById('stock-store-confirm-copy');
  const okBtn = document.getElementById('stock-store-confirm-ok');
  if (copy) {
    copy.textContent = reinstall
      ? `Reinstall ${name || slug} on the connected Neo?\n\nThis replaces the build already installed.${how}\n\nThis switches briefly to manager mode, then returns to keyboard mode.`.trim()
      : `Install ${name || slug} onto the connected Neo?\n\n${detail}${how}\n\nThis switches briefly to manager mode, then returns to keyboard mode.`.trim();
  }
  if (okBtn) okBtn.textContent = reinstall ? 'Reinstall on Neo' : 'Install on Neo';
  if (panel) {
    panel.hidden = false;
    panel.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
  }
  queueMicrotask(() => document.getElementById('stock-store-confirm-ok')?.focus());
}

async function installStockApplet(slug, name) {
  if (!slug || stockStoreInstallBusy) return;
  hideStockStoreConfirm();
  if (!(await ensureManagerMode(`Installing ${name || slug}`, { confirm: false }))) return;

  const installBtn = document.querySelector(
    `#stock-applet-list [data-stock-action="install"][data-stock-slug="${typeof CSS !== 'undefined' && CSS.escape ? CSS.escape(String(slug)) : String(slug).replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"]`
  );
  stockStoreInstallBusy = true;
  setButtonBusy(installBtn, true, 'Installing…');
  setStockStoreStatus(`Installing ${name || slug}…`, 'info');
  try {
    await apiRequest(`/neo/stock-applets/${encodeURIComponent(slug)}/install`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: '{}',
    });
    const app = stockStoreCache.find((a) => a.slug === slug);
    const appletId = Number(app?.applet_id);
    if (Number.isFinite(appletId) && appletId > 0) {
      installedAppletIds.add(appletId);
    }
    applyStockStoreFilter();
    const done = `${name || slug} installed on the Neo. Open it from Applets, or Reinstall to replace it.`;
    setStockStoreStatus(done, 'success');
    showNotice(done, 'success');
    try {
      await refreshApplets({ confirm: false });
    } catch (_) { /* card state already updated from applet_id */ }
    await refreshStatus();
  } catch (error) {
    const msg = String(error.message || error);
    let notice = msg || 'Install failed';
    let type = 'error';
    if (msg.includes('already_installed')) {
      notice = 'Already installed — use Reinstall to replace this build, or remove it first.';
      type = 'warning';
      const app = stockStoreCache.find((a) => a.slug === slug);
      const appletId = Number(app?.applet_id);
      if (Number.isFinite(appletId) && appletId > 0) {
        installedAppletIds.add(appletId);
        applyStockStoreFilter();
      }
    } else if (msg.includes('neo_not_connected')) {
      notice = 'Connect the Neo by USB, then install again.';
      type = 'warning';
    } else if (msg.includes('insufficient_space')) {
      notice = 'Not enough free ROM or RAM on the Neo. Remove an applet and retry.';
      type = 'warning';
    } else if (msg.includes('not_bundled')) {
      notice = 'That applet is not bundled in this firmware image.';
      type = 'warning';
    }
    setStockStoreStatus(notice, type);
    showNotice(notice, type);
  } finally {
    stockStoreInstallBusy = false;
    setButtonBusy(installBtn, false);
  }
}

async function refreshNeoFiles(opts = {}) {
  const button = document.querySelector('#refresh-neo-files');
  const guidance = document.querySelector('#neo-documents-guidance');
  if (!getAuthToken()) {
    guidance.textContent = 'Sign in to scan documents on the connected NEO.';
    return;
  }
  if (!(await ensureManagerMode('Scanning Neo documents', { confirm: !!opts.confirm }))) return;
  setButtonBusy(button, true, 'Scanning...');
  guidance.textContent = 'Scanning AlphaWord document slots (keyboard mode paused)…';
  try {
    const response = await authFetch('/neo/files');
    if (response.status === 401) {
      setAuthToken(null);
      updateSignInState();
      throw new Error('Session expired — sign in again, then retry the scan.');
    }
    if (response.status === 412) {
      throw new Error('The NEO is not connected by USB. Connect it on OTG1, then try again.');
    }
    if (response.status === 404) {
      throw new Error('This firmware build does not include the NEO file scanner yet. Reflash firmware and the web portal partition.');
    }
    if (!response.ok) {
      const errBody = await response.text().catch(() => '');
      throw new Error(errBody || 'The NEO did not return a document list.');
    }
    const files = await response.json();
    const alphaWordFiles = Array.isArray(files)
      ? files.filter((file) => Number(file.applet_id || NEO_ALPHAWORD_ID) === NEO_ALPHAWORD_ID)
      : [];
    /* used_size = exportable text bytes (firmware); 0 means pad-only / empty — hide like backup. */
    const isEmptySlot = (file) => Number(file.used_size ?? 0) <= 0;
    const emptySlots = alphaWordFiles.filter(isEmptySlot).length;
    const documents = alphaWordFiles.filter((file) => !isEmptySlot(file));
    renderNeoFiles(documents);
    if (documents.length) {
      guidance.textContent = emptySlots
        ? `${documents.length} AlphaWord document${documents.length === 1 ? '' : 's'} with text · ${emptySlots} empty slot${emptySlots === 1 ? '' : 's'} hidden.`
        : `${documents.length} AlphaWord document${documents.length === 1 ? '' : 's'} on the connected NEO.`;
    } else {
      guidance.textContent = emptySlots
        ? `No text documents — ${emptySlots} empty AlphaWord slot${emptySlots === 1 ? '' : 's'} hidden.`
        : 'No AlphaWord documents were reported by the connected NEO.';
    }
  } catch (error) {
    neoFilesEmpty.hidden = false;
    neoFileTableWrap.hidden = true;
    neoFilesEmpty.querySelector('h3').textContent = 'Device file scan unavailable';
    neoFilesEmpty.querySelector('p').textContent = error.message || 'Connect the NEO by USB, then try again.';
    guidance.textContent = error.message || 'The workspace is ready for the authenticated NEO file-list API.';
  } finally {
    setButtonBusy(button, false);
  }
}

function renderNeoFiles(files) {
  neoFilesEmpty.hidden = files.length > 0;
  neoFileTableWrap.hidden = files.length === 0;
  if (files.length === 0) {
    neoFileList.innerHTML = '';
    const emptyTitle = neoFilesEmpty?.querySelector('h3');
    const emptyCopy = neoFilesEmpty?.querySelector('p');
    if (emptyTitle) emptyTitle.textContent = 'No documents with text';
    if (emptyCopy) {
      emptyCopy.textContent =
        'Empty AlphaWord slots (512 B reserved, no text) are hidden from this list.';
    }
    return;
  }
  neoFileList.innerHTML = files.map((file) => {
    const usedSize = Number(file.used_size ?? 0);
    const allocatedSize = Number(file.alloc_size || file.allocated_size || 0);
    const isLarge = usedSize >= LOCAL_BACKUP_WARNING_BYTES;
    const fileIndex = Number(file.file_index || file.index || 0);
    const appletId = NEO_ALPHAWORD_ID;
    const rowClass = isLarge ? 'is-large' : '';
    const sizeLabel =
      allocatedSize > usedSize
        ? `${formatBytes(usedSize)} · ${formatBytes(allocatedSize)} reserved`
        : formatBytes(usedSize);
    return `<tr${rowClass ? ` class="${rowClass}"` : ''}><td><strong>${escapeHtml(file.name || 'Untitled')}</strong>${isLarge ? '<small class="file-warning">Review before restoring</small>' : ''}</td><td>${fileIndex || '—'}</td><td>${sizeLabel}</td><td class="table-actions"><button class="table-action" type="button" data-file-action="read" data-applet-id="${appletId}" data-file-index="${fileIndex}" data-file-name="${escapeHtml(file.name || '')}">Read</button><button class="table-action" type="button" data-file-action="download" data-applet-id="${appletId}" data-file-index="${fileIndex}">Download</button><button class="table-action" type="button" data-file-action="write" data-applet-id="${appletId}" data-file-index="${fileIndex}" data-file-name="${escapeHtml(file.name || '')}">Write</button><button class="table-action danger-action" type="button" data-file-action="clear" data-applet-id="${appletId}" data-file-index="${fileIndex}" data-file-name="${escapeHtml(file.name || '')}">Clear</button></td></tr>`;
  }).join('');
}

const FLASH_CARDS_APPLET_ID = 0xA1B6;

function isFlashCardsApplet(applet) {
  const id = Number(applet?.id ?? applet);
  if (id === FLASH_CARDS_APPLET_ID) return true;
  const name = String(applet?.name || '').toLowerCase();
  return name.includes('flash card');
}

function renderApplet(applet) {
  const id = Number(applet.id);
  const rawName = (applet.name || `Applet ${id}`).toString();
  const title = escapeHtml(rawName);
  const details = `ID ${id} · ${applet.file_count} file${applet.file_count === 1 ? '' : 's'} · ${formatBytes(applet.rom_size)} ROM`;
  const settingsBtn = isFlashCardsApplet(applet)
    ? `<button class="table-action" type="button" data-applet-action="flashdeck-settings" data-applet-id="${id}" title="Open Flash Cards deck library">Settings</button>`
    : '';
  /* Encode name for data-* so Download saves a real filename (not HTML entities). */
  return `<div class="action-row applet-row"><span class="action-icon blue">APP</span><span><b>${title}</b><small>${details}</small></span><span class="applet-actions">${settingsBtn}<button class="table-action" type="button" data-applet-action="download" data-applet-id="${id}" data-applet-name="${encodeURIComponent(rawName)}">Download</button><button class="table-action danger-action" type="button" data-applet-action="remove" data-applet-id="${id}" data-applet-name="${encodeURIComponent(rawName)}">Remove</button></span></div>`;
}

async function openStockStoreDialog() {
  const dlg = document.getElementById('stock-store-dialog');
  if (!dlg || typeof dlg.showModal !== 'function') {
    showNotice('Applet Store dialog is missing from this portal build.', 'error');
    return;
  }
  /* Open immediately (same pattern as Logs). Do not dismiss on backdrop —
     that classic dialog bug made Install from store look like a dead button. */
  try {
    if (!dlg.open) dlg.showModal();
  } catch (error) {
    showNotice(`Could not open Applet Store: ${error.message || error}`, 'error');
    return;
  }
  hideStockStoreConfirm();
  setStockStoreStatus('', 'info');
  const empty = document.querySelector('#stock-applets-empty');
  const list = document.querySelector('#stock-applet-list');
  const toolbar = document.querySelector('#stock-store-toolbar');
  if (toolbar) toolbar.hidden = true;
  if (list) {
    list.hidden = true;
    list.innerHTML = '';
  }
  if (empty) {
    empty.hidden = false;
    const h = empty.querySelector('h3');
    const p = empty.querySelector('p');
    if (h) h.textContent = 'Loading catalog…';
    if (p) p.textContent = 'Stock applets appear here when the buddy responds.';
  }
  const search = document.querySelector('#stock-store-search');
  if (search) {
    search.value = stockStoreQuery;
    queueMicrotask(() => {
      try { search.focus(); } catch (_) { /* ignore */ }
    });
  }
  try {
    await refreshStockApplets();
    /* Sync Installed badges from the Neo applet list (best-effort). */
    try {
      await refreshApplets({ confirm: false });
    } catch (_) { /* catalog still usable without live install state */ }
  } catch (error) {
    setStockStoreStatus(error.message || 'Could not load catalog', 'error');
    showNotice(error.message || 'Could not load Applet Store catalog', 'error');
  }
}
window.openStockStoreDialog = openStockStoreDialog;

async function openFlashDeckBuilder() {
  const dlg = document.getElementById('flashdeck-dialog');
  if (!dlg) return;
  await new Promise((resolve) => requestAnimationFrame(resolve));
  try {
    if (!dlg.open) dlg.showModal();
  } catch (_) {
    return;
  }
  await refreshFlashDecks({ keepDraft: true });
}

function openAppletDialog(operation, details = {}) {
  appletDialogOperation = operation;
  const install = operation === 'install';
  document.querySelector('#applet-dialog-label').textContent = install ? 'SMARTAPPLET INSTALL' : 'SMARTAPPLET REMOVAL';
  document.querySelector('#applet-dialog-title').textContent = install ? 'Install SmartApplet' : `Remove ${details.name || 'SmartApplet'}?`;
  document.querySelector('#applet-install-fields').hidden = !install;
  document.querySelector('#applet-submit').textContent = install ? 'Install applet' : 'Remove applet';
  document.querySelector('#applet-dialog-copy').textContent = install
    ? 'Choose a validated .os3kapp package. The NEO checks the header and free ROM/RAM before writing. This switches briefly to manager mode, then returns to keyboard mode.'
    : 'This permanently removes the selected SmartApplet and its files from the NEO. This cannot be undone.';
  setDialogStatus(document.querySelector('#applet-dialog-status'), '', '');
  const fileInput = document.querySelector('#applet-file');
  if (fileInput) {
    fileInput.value = '';
    fileInput.required = install;
  }
  const fileName = document.querySelector('#applet-file-name');
  if (fileName) fileName.textContent = 'Choose a package file';
  const replace = document.querySelector('#applet-replace');
  if (replace) replace.checked = false;
  appletDialog.dataset.appletId = details.id || '';
  appletDialog.showModal();
}

async function invokeFileAction(action, appletId, fileIndex, fileName = '') {
  if (action === 'clear') {
    if (!window.confirm(`Clear "${fileName || `file ${fileIndex}`}" on the NEO? This cannot be undone.`)) return;
    if (!(await ensureManagerMode('Clearing a Neo file'))) return;
    try {
      await apiRequest(`/neo/applets/${appletId}/files/${fileIndex}`, { method: 'DELETE' });
      showNotice('Document cleared on the NEO.', 'success');
      await refreshNeoFiles({ confirm: false });
    } catch (error) {
      showNotice(`Clear failed: ${error.message}`, 'error');
    }
    return;
  }
  if (action === 'write') {
    if (!(await ensureManagerMode('Writing to a Neo file'))) return;
    activeWriteTarget = { appletId: Number(appletId), fileIndex: Number(fileIndex), fileName };
    document.querySelector('#write-document-title').textContent = fileName || `File ${fileIndex}`;
    document.querySelector('#write-document-meta').textContent = `Applet ${appletId} · file ${fileIndex}`;
    document.querySelector('#write-document-content').value = '';
    document.querySelector('#write-document-status').textContent = '';
    writeDocumentDialog.showModal();
    return;
  }
  if (action === 'read' || action === 'download') {
    if (!(await ensureManagerMode(action === 'read' ? 'Reading a Neo file' : 'Downloading a Neo file'))) {
      return;
    }
  }
  const mapQuery = neoCharmapQuery('?');
  const path = `/neo/applets/${appletId}/files/${fileIndex}/${action}${action === 'download' || action === 'read' ? mapQuery : ''}`;
  try {
    const response = await apiRequest(path, { method: action === 'read' ? 'POST' : 'GET' });
    if (action === 'read') {
      const contentType = response.headers.get('content-type') || '';
      if (contentType.includes('application/json')) {
        const result = await response.json();
        showNotice(result.saved ? 'Document backed up locally.' : 'Document read completed.', 'success');
        await refreshFiles();
      } else {
        activeDocument = { appletId, fileIndex, content: await response.text() };
        document.querySelector('#document-dialog-title').textContent = `Document ${fileIndex}`;
        document.querySelector('#document-dialog-meta').textContent = `Applet ${appletId} · file ${fileIndex}`;
        document.querySelector('#document-dialog-content').textContent = activeDocument.content || 'This document is empty.';
        documentDialog.showModal();
      }
      return;
    }
    const blob = await response.blob();
    const downloadUrl = URL.createObjectURL(blob);
    const link = Object.assign(document.createElement('a'), { href: downloadUrl, download: `neo-file-${fileIndex}.txt` });
    link.click();
    URL.revokeObjectURL(downloadUrl);
  } catch (error) {
    showNotice(`File ${action} is unavailable: ${error.message || 'USB transport is not ready.'}`, 'error');
  }
}

let liveSequence = -1;
let liveTextContent = '';
let livePaused = false;
let liveRaw = false;
let liveFollow = true; // when true, autoscrolls to newest

function setFollowUI(enabled) {
  liveFollow = !!enabled;
  const followEl = document.getElementById('follow-indicator');
  const pauseBtn = document.getElementById('pause-live');
  if (followEl) {
    followEl.textContent = liveFollow ? (livePaused ? 'Paused' : 'Follow') : 'Hold';
    followEl.classList.toggle('paused', livePaused || !liveFollow);
  }
  if (pauseBtn) {
    pauseBtn.textContent = livePaused ? 'Resume' : (liveFollow ? 'Pause' : 'Paused');
    pauseBtn.classList.toggle('paused', livePaused || !liveFollow);
  }
}
function renderLiveTextBody(text) {
  const lines = (text || '').split(/\r?\n/);
  let html = '';
  for (let i = 0; i < lines.length; ++i) {
    const cls = (i === lines.length - 1) ? 'live-line new' : 'live-line';
    const content = lines[i] === '' ? '\u00A0' : escapeHtml(lines[i]);
    html += `<div class="${cls}">${content}</div>`;
  }
  if (!html) html = '<div class="live-line">Waiting for NEO keyboard input...</div>';
  if (liveText) {
    liveText.innerHTML = html;
    if (liveFollow) liveText.scrollTop = liveText.scrollHeight;
  }
}

async function refreshLiveText() {
  if (!IS_TYPING_PAGE || livePaused || !liveText) return;
  if (!sNeoKeyboardActive && !liveRaw) {
    liveText.innerHTML = '<div class="live-line">Neo is in manager mode — live typing resumes when keyboard mode returns. <a href="index.html">Back to documents</a></div>';
    return;
  }
  try {
    if (liveRaw) {
      const limit = 32;
      const res = await localFetch(`/keyboard/raw?limit=${limit}`);
      if (!res.ok) {
        liveText.innerHTML = `<div class="live-line live-error">Raw HID view unavailable (${res.status}).</div>`;
        return;
      }
      const arr = await res.json().catch(() => []);
      if (!Array.isArray(arr)) return;
      const html = arr.map((e) => {
        const d = new Date((e.ts || 0) * 1000);
        const t = d.toLocaleTimeString();
        return `<div class="live-line"><div class="live-meta"><span class="live-ts">${t}</span></div><div class="live-hex">${escapeHtml(e.data_hex || '')}</div></div>`;
      }).join('');
      liveText.innerHTML = html || '<div class="live-line">No recent raw HID reports</div>';
      if (liveFollow) liveText.scrollTop = liveText.scrollHeight;
      return;
    }

    const response = await localFetch('/keyboard/recent');
    if (!response.ok) {
      liveText.innerHTML = `<div class="live-line live-error">Live keyboard feed unavailable (${response.status}). Check that firmware and portal are up to date.</div>`;
      return;
    }
    const payload = await response.json().catch(() => null);
    if (!payload || typeof payload.text !== 'string') {
      liveText.innerHTML = '<div class="live-line live-error">Unexpected live keyboard response from the device.</div>';
      return;
    }
    const sequence = Number(payload.sequence);
    if (Number.isFinite(sequence) && sequence === liveSequence && payload.text === liveTextContent) {
      return;
    }
    if (Number.isFinite(sequence)) liveSequence = sequence;
    liveTextContent = payload.text;
    renderLiveTextBody(payload.text);
  } catch (error) {
    liveText.innerHTML = `<div class="live-line live-error">${escapeHtml(error.message || 'Could not reach the live keyboard feed.')}</div>`;
  }
}

document.querySelector('#refresh-files')?.addEventListener('click', refreshFiles);
document.querySelector('#refresh-applets')?.addEventListener('click', () => refreshApplets({ confirm: true }));
document.querySelector('#refresh-stock-applets')?.addEventListener('click', () => refreshStockApplets());
document.querySelector('#install-from-store')?.addEventListener('click', (event) => {
  event.preventDefault();
  void openStockStoreDialog();
});
document.querySelector('#stock-store-close')?.addEventListener('click', () => {
  hideStockStoreConfirm();
  document.getElementById('stock-store-dialog')?.close();
});
document.querySelector('#stock-store-dialog')?.addEventListener('cancel', () => {
  hideStockStoreConfirm();
});
document.querySelector('#stock-store-dialog')?.addEventListener('close', () => {
  hideStockStoreConfirm();
});
document.querySelector('#stock-store-confirm-cancel')?.addEventListener('click', () => {
  hideStockStoreConfirm();
});
document.querySelector('#stock-store-confirm-ok')?.addEventListener('click', () => {
  const pending = stockStorePendingInstall;
  if (!pending) return;
  installStockApplet(pending.slug, pending.name);
});
document.querySelector('#flashdeck-close')?.addEventListener('click', () => {
  document.getElementById('flashdeck-dialog')?.close();
});
document.querySelector('#flashdeck-dialog')?.addEventListener('click', (event) => {
  if (event.target === event.currentTarget) event.currentTarget.close();
});
document.querySelector('#stock-store-filters')?.addEventListener('click', (event) => {
  const btn = event.target.closest('[data-store-filter]');
  if (!btn) return;
  stockStoreFilter = btn.dataset.storeFilter || 'all';
  applyStockStoreFilter();
});
document.querySelector('#stock-store-search')?.addEventListener('input', (event) => {
  stockStoreQuery = event.target.value || '';
  applyStockStoreFilter();
});
document.querySelector('#stock-applet-list')?.addEventListener('click', (event) => {
  const editBtn = event.target.closest('[data-stock-action="edit-decks"]');
  if (editBtn) {
    hideStockStoreConfirm();
    document.getElementById('stock-store-dialog')?.close();
    openFlashDeckBuilder();
    return;
  }
  const btn = event.target.closest('[data-stock-action="install"]');
  if (!btn || btn.disabled) return;
  const row = btn.closest('.store-card');
  const name = row?.querySelector('.store-card-title')?.textContent || btn.dataset.stockSlug;
  promptStockStoreInstall(btn.dataset.stockSlug, name);
});
document.querySelector('#flashdeck-refresh')?.addEventListener('click', () => refreshFlashDecks());
document.querySelector('#flashdeck-new')?.addEventListener('click', () => newFlashDeck());
document.querySelector('#flashdeck-import')?.addEventListener('click', () => {
  const input = document.querySelector('#flash-deck-file');
  if (input) {
    input.value = '';
    input.click();
  }
});
document.querySelector('#flash-deck-file')?.addEventListener('change', (event) => {
  const file = event.target.files?.[0];
  importFlashDeckToLibrary(file);
});
document.querySelector('#flashdeck-save')?.addEventListener('click', () => saveFlashDeck());
document.querySelector('#flashdeck-push')?.addEventListener('click', () => pushFlashDeck());
document.querySelector('#flashdeck-delete')?.addEventListener('click', () => deleteFlashDeck());
document.querySelector('#flashdeck-add-card')?.addEventListener('click', () => {
  syncFlashDraftFromDom();
  if (!flashDeckDraft) return;
  if ((flashDeckDraft.cards?.length || 0) >= 16) {
    showNotice('Maximum 16 cards per set.', 'warning');
    return;
  }
  flashDeckDraft.cards = [...(flashDeckDraft.cards || []), { front: '', back: '' }];
  renderFlashEditor();
  const lastFront = document.querySelector('#flashdeck-cards .flashdeck-card-row:last-child [data-flash-side="front"]');
  queueMicrotask(() => {
    try { lastFront?.focus(); } catch (_) { /* ignore */ }
  });
});
document.querySelector('#flashdeck-set-list')?.addEventListener('click', (event) => {
  const btn = event.target.closest('[data-flash-id]');
  if (!btn) return;
  loadFlashDeck(btn.dataset.flashId);
});
document.querySelector('#flashdeck-cards')?.addEventListener('click', (event) => {
  const remove = event.target.closest('[data-flash-remove-card]');
  if (!remove) return;
  syncFlashDraftFromDom();
  if (!flashDeckDraft) return;
  const row = remove.closest('.flashdeck-card-row');
  const idx = Number(row?.dataset.cardIndex);
  if (!Number.isFinite(idx)) return;
  flashDeckDraft.cards.splice(idx, 1);
  if (!flashDeckDraft.cards.length) flashDeckDraft.cards = [{ front: '', back: '' }];
  renderFlashEditor();
});
document.querySelector('#refresh-neo-files')?.addEventListener('click', () => refreshNeoFiles({ confirm: true }));
document.querySelector('#neo-read-all')?.addEventListener('click', backupAllNeoFiles);
document.querySelector('#neo-backup-now')?.addEventListener('click', backupNowNeoFiles);
document.querySelector('#neo-manager')?.addEventListener('click', enterManagerMode);
document.querySelector('#remove-all-applets')?.addEventListener('click', removeAllApplets);
document.querySelector('#install-applet')?.addEventListener('click', () => openAppletDialog('install'));
document.querySelector('#applet-close')?.addEventListener('click', () => appletDialog.close());
document.querySelector('#applet-cancel')?.addEventListener('click', () => appletDialog.close());
document.querySelector('#applet-file')?.addEventListener('change', async (event) => {
  const file = event.target.files[0];
  document.querySelector('#applet-file-name').textContent = file?.name || 'Choose a package file';
  const status = document.querySelector('#applet-dialog-status');
  if (!file || appletDialogOperation !== 'install' || !getAuthToken()) return;
  setDialogStatus(status, 'Inspecting package header…', 'info');
  try {
    const response = await apiRequest('/neo/applets/inspect', {
      method: 'POST',
      body: file,
      headers: { 'Content-Type': 'application/octet-stream' }
    });
    const info = await response.json();
    setDialogStatus(
      status,
      `${info.name} · ID ${info.applet_id} · ${formatBytes(info.rom_size)} ROM · ${info.file_count} file${info.file_count === 1 ? '' : 's'}`,
      'success'
    );
  } catch (error) {
    setDialogStatus(status, error.message || 'Could not inspect the package.', 'error');
  }
});
document.querySelector('#write-document-close')?.addEventListener('click', () => writeDocumentDialog.close());
document.querySelector('#write-document-cancel')?.addEventListener('click', () => writeDocumentDialog.close());
writeDocumentForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!activeWriteTarget) return;
  const status = document.querySelector('#write-document-status');
  const text = document.querySelector('#write-document-content').value;
  if (!(await ensureManagerMode('Writing to a Neo file', { confirm: false }))) return;
  status.textContent = 'Writing to the NEO…';
  try {
    await apiRequest(
      `/neo/applets/${activeWriteTarget.appletId}/files/${activeWriteTarget.fileIndex}/write${neoCharmapQuery('?')}`,
      { method: 'POST', body: text, headers: { 'Content-Type': 'text/plain; charset=utf-8' } }
    );
    writeDocumentDialog.close();
    showNotice('Document written to the NEO.', 'success');
    await refreshNeoFiles({ confirm: false });
  } catch (error) {
    status.textContent = error.message || 'Write failed.';
  }
});
document.querySelector('#document-close')?.addEventListener('click', () => documentDialog.close());
document.querySelector('#document-download')?.addEventListener('click', () => {
  if (!activeDocument) return;
  const blob = new Blob([activeDocument.content], { type: 'text/plain;charset=utf-8' });
  const downloadUrl = URL.createObjectURL(blob);
  const link = Object.assign(document.createElement('a'), { href: downloadUrl, download: `neo-file-${activeDocument.fileIndex}.txt` });
  link.click();
  URL.revokeObjectURL(downloadUrl);
});
document.querySelector('#document-save')?.addEventListener('click', async () => {
  if (!activeDocument) return;
  if (!(await ensureManagerMode('Backing up a Neo file', { confirm: false }))) return;
  const button = document.querySelector('#document-save');
  setButtonBusy(button, true, 'Saving...');
  try {
    await apiRequest(`/neo/applets/${activeDocument.appletId}/files/${activeDocument.fileIndex}/read?backup=1`, { method: 'POST' });
    documentDialog.close();
    showNotice('Document saved as a local backup.', 'success');
    refreshFiles();
  } catch (error) {
    showNotice(`Backup failed: ${error.message}`, 'error');
  } finally {
    setButtonBusy(button, false);
  }
});
document.querySelector('#neo-file-list')?.addEventListener('click', (event) => {
  const button = event.target.closest('[data-file-action]');
  if (button) invokeFileAction(button.dataset.fileAction, button.dataset.appletId, button.dataset.fileIndex, button.dataset.fileName);
});
document.querySelector('#applet-list')?.addEventListener('click', (event) => {
  const button = event.target.closest('[data-applet-action]');
  if (!button) return;
  const rawName = button.dataset.appletName ? decodeURIComponent(button.dataset.appletName) : '';
  if (button.dataset.appletAction === 'flashdeck-settings') {
    openFlashDeckBuilder();
    return;
  }
  if (button.dataset.appletAction === 'remove') openAppletDialog('remove', { id: button.dataset.appletId, name: rawName });
  if (button.dataset.appletAction === 'download') invokeAppletDownload(button.dataset.appletId, rawName);
});
appletForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  const leaveLabel = appletDialogOperation === 'install' ? 'Installing a SmartApplet' : 'Removing a SmartApplet';
  if (!(await ensureManagerMode(leaveLabel))) return;
  const status = document.querySelector('#applet-dialog-status');
  const submitButton = document.querySelector('#applet-submit');
  setDialogStatus(
    status,
    appletDialogOperation === 'install'
      ? 'Uploading and installing on the NEO… this can take a minute.'
      : 'Contacting the NEO...',
    'info'
  );
  setButtonBusy(submitButton, true, appletDialogOperation === 'install' ? 'Installing...' : 'Removing...');
  try {
    if (appletDialogOperation === 'install') {
      const file = document.querySelector('#applet-file').files[0];
      if (!file) throw new Error('Choose a SmartApplet package first.');
      if (file.size > 1024 * 1024) {
        throw new Error('The package is too large to upload (maximum 1 MB).');
      }
      const replace = !!document.querySelector('#applet-replace')?.checked;
      await apiRequest('/neo/applets', {
        method: 'POST',
        body: file,
        headers: {
          'Content-Type': 'application/octet-stream',
          'X-Neo-Replace': replace ? 'true' : 'false',
        },
      });
      appletDialog.close();
      showNotice('SmartApplet installed.', 'success');
      await refreshApplets({ confirm: false });
      /* Install + list leave Neo in manager mode; return to keyboard like download. */
      try {
        await apiRequest('/neo/restart', { method: 'POST' });
        showNotice('SmartApplet installed. Neo returned to keyboard mode.', 'success');
      } catch (_) {
        showNotice('SmartApplet installed. Use Keyboard mode on Typing & Bluetooth if typing does not resume.', 'info');
      }
      await refreshStatus();
      return;
    } else {
      await apiRequest(`/neo/applets/${appletDialog.dataset.appletId}`, { method: 'DELETE' });
      appletDialog.close();
      showNotice('SmartApplet removed.', 'success');
    }
    await refreshApplets({ confirm: false });
    await refreshStatus();
  } catch (error) {
    const raw = error?.message || 'The operation could not be completed.';
    const already =
      /already installed/i.test(raw) ||
      /already_installed/i.test(raw) ||
      raw.trim() === '{"error":"already_installed"}';
    if (already && appletDialogOperation === 'install') {
      const replace = document.querySelector('#applet-replace');
      if (replace) replace.checked = true;
      setDialogStatus(
        status,
        'That applet is already on the Neo. “Replace” is now checked — tap Install again to overwrite it.',
        'error'
      );
    } else {
      setDialogStatus(status, raw, 'error');
    }
  } finally {
    setButtonBusy(submitButton, false);
  }
});

async function invokeAppletDownload(appletId, appletName) {
  if (!(await ensureManagerMode('Downloading a SmartApplet'))) return;
  try {
    const response = await apiRequest(`/neo/applets/${appletId}/download`);
    const blob = await response.blob();
    let filename = `${(appletName || `applet-${appletId}`).replace(/[\\/:*?"<>|]+/g, '_').trim() || `applet-${appletId}`}.os3kapp`;
    if (Number(appletId) === 0) {
      filename = 'SystemRom.os3kos';
    }
    const cd = response.headers.get('Content-Disposition') || '';
    const m = /filename=\"([^\"]+)\"/i.exec(cd);
    if (m && m[1]) {
      filename = m[1];
    }
    const downloadUrl = URL.createObjectURL(blob);
    const link = Object.assign(document.createElement('a'), { href: downloadUrl, download: filename });
    link.click();
    URL.revokeObjectURL(downloadUrl);
    showNotice(`Downloaded ${filename} (${formatBytes(blob.size)}). Neo returned to keyboard mode.`, 'success');
    await refreshStatus();
  } catch (error) {
    showNotice(`Applet download unavailable: ${error.message || 'USB transport is not ready.'}`, 'error');
  }
}

async function backupAllNeoFiles() {
  const button = document.querySelector('#neo-read-all');
  if (sBackupBusy) {
    showNotice('A backup is already running.', 'error');
    return;
  }
  if (!(await ensureManagerMode('Backup all'))) return;
  setButtonBusy(button, true, 'Backing up…');
  try {
    const response = await apiRequest(`/neo/applets/${NEO_ALPHAWORD_ID}/files/read-all${neoCharmapQuery('?')}`, { method: 'POST' });
    const result = await response.json();
    const kb = result.returned_to_keyboard ? ' Neo returned to keyboard mode.' : ' Could not return Neo to keyboard — use Keyboard mode on Typing & Bluetooth.';
    showNotice(`Force-backed up ${result.count || 0} AlphaWord document${result.count === 1 ? '' : 's'} (every non-empty slot).${kb}`, result.returned_to_keyboard ? 'success' : 'error');
    await refreshFiles();
    await refreshStatus();
  } catch (error) {
    showNotice(`Backup all failed: ${error.message}`, 'error');
  } finally {
    setButtonBusy(button, false);
  }
}

async function backupNowNeoFiles() {
  const button = document.querySelector('#neo-backup-now');
  if (sBackupBusy) {
    showNotice('A backup is already running.', 'error');
    return;
  }
  if (!(await ensureManagerMode('Backup now'))) return;
  setButtonBusy(button, true, 'Starting…');
  try {
    await apiRequest('/neo/autobackup', { method: 'POST' });
    showNotice('Backup started — only files that changed since today’s save, then keyboard mode.', 'success');
    sAwaitingBackupFinish = true;
    setBackupBusyUi(true);
    await refreshStatus();
  } catch (error) {
    showNotice(`Backup now failed: ${error.message}`, 'error');
  } finally {
    setButtonBusy(button, false);
  }
}

async function enterManagerMode() {
  const button = document.querySelector('#neo-manager');
  setButtonBusy(button, true, 'Switching…');
  try {
    if (!(await ensureManagerMode('Manager mode'))) return;
    showNotice('Neo is in manager mode.', 'success');
  } finally {
    setButtonBusy(button, false);
  }
}

async function restartNeo() {
  const button = document.querySelector('#neo-restart');
  if (sBackupBusy) {
    showNotice('Wait for the backup to finish first.', 'error');
    return;
  }
  if (!window.confirm('Return the NEO to keyboard mode? USB file operations will stop until you reconnect.')) return;
  setButtonBusy(button, true, 'Restarting…');
  try {
    await apiRequest('/neo/restart', { method: 'POST' });
    showNotice('NEO restarted into keyboard mode.', 'success');
    refreshStatus();
  } catch (error) {
    showNotice(`Restart failed: ${error.message}`, 'error');
  } finally {
    setButtonBusy(button, false);
  }
}

async function removeAllApplets() {
  if (!(await ensureManagerMode('Removing all SmartApplets'))) return;
  if (!window.confirm('Remove ALL SmartApplets from the NEO? The device will reboot and this cannot be undone.')) return;
  const button = document.querySelector('#remove-all-applets');
  setButtonBusy(button, true, 'Removing…');
  try {
    await apiRequest('/neo/applets', { method: 'DELETE' });
    showNotice('All SmartApplets removed from the NEO.', 'success');
    await refreshApplets({ confirm: false });
    await refreshNeoFiles({ confirm: false });
  } catch (error) {
    showNotice(`Remove all failed: ${error.message}`, 'error');
  } finally {
    setButtonBusy(button, false);
  }
}
document.querySelector('#clear-live')?.addEventListener('click', async () => {
  try {
    await authFetch('/keyboard/clear', {method: 'POST'});
    liveSequence = -1;
    liveTextContent = '';
    // Clear UI immediately
    const el = document.getElementById('live-text');
    if (el) el.innerHTML = '<div class="live-line">Monitor cleared.</div>';
    refreshLiveText();
  } catch (error) {
    // The control becomes active once the firmware endpoint is available.
  }
});

// Pause/unpause live updates
document.querySelector('#pause-live')?.addEventListener('click', (ev) => {
  livePaused = !livePaused;
  if (livePaused) {
    // pause updates and disable follow so UI doesn't auto-scroll
    setFollowUI(false);
  } else {
    // resume and enable follow
    setFollowUI(true);
    refreshLiveText();
  }
});

// Make follow indicator clickable for compact toggle
document.querySelector('#follow-indicator')?.addEventListener('click', () => {
  setFollowUI(!liveFollow);
});

// initialize UI state
if (IS_TYPING_PAGE) setFollowUI(true);

// Toggle raw HID view
document.querySelector('#toggle-raw')?.addEventListener('click', function (ev) {
  liveRaw = !liveRaw;
  ev.currentTarget.textContent = liveRaw ? 'Decoded' : 'Raw';
  // When switching modes, clear sequence to force refresh
  liveSequence = -1;
  liveTextContent = '';
  refreshLiveText();
});

document.querySelector('#setup-later')?.addEventListener('click', () => {
  localStorage.setItem('neo2_setup_complete', 'true');
  setupDialog.close();
});

document.querySelector('#setup-form')?.addEventListener('submit', (event) => {
  event.preventDefault();
  localStorage.setItem('neo2_device_name', document.querySelector('#setup-device-name').value.trim() || 'Neo2 Buddy');
  localStorage.setItem('neo2_setup_complete', 'true');
  const configureCloud = document.querySelector('#setup-cloud-enabled').checked;
  setupDialog.close();
  if (configureCloud) {
    loadCloudSyncConfig().then(() => syncDialog.showModal());
  }
});

document.querySelector('#open-sync')?.addEventListener('click', async () => {
  await loadCloudSyncConfig();
  syncDialog.showModal();
});
document.querySelector('#sync-cancel')?.addEventListener('click', () => syncDialog.close());
document.querySelectorAll('input[name="provider"]').forEach((input) => input.addEventListener('change', syncProviderFieldsUi));
document.querySelector('#sync-test')?.addEventListener('click', async () => {
  const form = document.querySelector('#sync-form');
  try {
    await testCloudSync(form);
    showNotice('Cloud connection test succeeded.', 'success');
  } catch (error) {
    showNotice(error.message, 'error');
  }
});
document.querySelector('#sync-run-now')?.addEventListener('click', async () => {
  const form = document.querySelector('#sync-form');
  try {
    await saveCloudSyncConfig(form);
    await runCloudSync();
    showNotice('Cloud upload started.', 'success');
  } catch (error) {
    showNotice(error.message, 'error');
  }
});
document.querySelector('#sync-run-backups')?.addEventListener('click', async () => {
  try {
    await runCloudSync();
    showNotice('Uploading backups to cloud…', 'success');
  } catch (error) {
    showNotice(error.message, 'error');
  }
});

async function fetchWifiScan() {
  const sel = document.querySelector('#wifi-ssid');
  if (!sel) return;
  const saved = sel.dataset.savedSsid || sel.value || '';
  const savedSet = new Set(savedWifiNetworks.map((n) => n.ssid).filter(Boolean));
  if (saved) savedSet.add(saved);
  sel.innerHTML = '<option value="">(scanning...)</option>';
  try {
    const res = await authFetch('/wifi/scan');
    if (!res.ok) throw new Error('scan failed');
    const arr = await res.json();
    const options = ['<option value="">(select a network)</option>'];
    const seen = new Set();
    for (const ssid of savedSet) {
      options.push(`<option value="${escapeHtml(ssid)}">${escapeHtml(ssid)} (saved)</option>`);
      seen.add(ssid);
    }
    for (const ap of arr) {
      const ssid = ap.ssid || '';
      if (!ssid || seen.has(ssid)) continue;
      seen.add(ssid);
      options.push(`<option value="${escapeHtml(ssid)}">${escapeHtml(ssid)} (${ap.rssi}dBm)</option>`);
    }
    sel.innerHTML = options.join('');
    if (saved) sel.value = saved;
  } catch (e) {
    const opts = ['<option value="">(scan failed)</option>'];
    for (const ssid of savedSet) {
      opts.push(`<option value="${escapeHtml(ssid)}">${escapeHtml(ssid)} (saved)</option>`);
    }
    sel.innerHTML = opts.join('');
    if (saved) sel.value = saved;
  }
}

document.querySelector('#wifi-scan-btn')?.addEventListener('click', () => fetchWifiScan());
document.querySelector('#wifi-add-btn')?.addEventListener('click', () => {
  const ssidSelect = document.querySelector('#wifi-ssid');
  const ssid = ssidSelect?.value.trim() || '';
  const password = document.querySelector('#wifi-password')?.value || '';
  if (!ssid) {
    showNotice('Scan and select a network to save.', 'error');
    return;
  }
  const existing = savedWifiNetworks.find((n) => n.ssid === ssid);
  if (!password && !(existing && existing.password_set)) {
    showNotice('Enter the Wi‑Fi password for this network.', 'error');
    return;
  }
  if (upsertSavedWifi(ssid, password)) {
    const wp = document.querySelector('#wifi-password');
    if (wp) wp.value = '';
    showNotice(`Saved “${ssid}”. Click Save preferences to apply.`, 'success');
  }
});
document.getElementById('wifi-saved-list')?.addEventListener('click', (event) => {
  const t = event.target;
  if (!(t instanceof HTMLElement)) return;
  const ssid = t.dataset.ssid;
  if (!ssid) return;
  if (t.classList.contains('wifi-saved-remove')) {
    removeSavedWifi(ssid);
    showNotice(`Removed “${ssid}” from the list (save to apply).`, 'success');
  } else if (t.classList.contains('wifi-saved-prefer')) {
    setPreferredWifi(ssid);
  }
});

document.querySelector('#sync-form')?.addEventListener('submit', async (event) => {
  event.preventDefault();
  try {
    await saveCloudSyncConfig(event.currentTarget);
    showNotice('Cloud destination saved on device.', 'success');
    syncDialog.close();
  } catch (error) {
    showNotice(error.message, 'error');
  }
});

function scheduleStatusPoll(delayMs) {
  if (statusPollTimer) clearTimeout(statusPollTimer);
  statusPollTimer = setTimeout(async () => {
    statusPollTimer = null;
    if (!document.hidden && getAuthToken()) {
      await refreshStatus();
    }
    const next = statusUnchangedStreak >= 2 ? STATUS_IDLE_MS : STATUS_ACTIVE_MS;
    scheduleStatusPoll(next);
  }, delayMs);
}

if (IS_DASHBOARD) {
  scheduleStatusPoll(STATUS_ACTIVE_MS);
}
document.addEventListener('visibilitychange', () => {
  if (!document.hidden && IS_DASHBOARD) {
    statusUnchangedStreak = 0;
    refreshStatus();
    scheduleStatusPoll(STATUS_ACTIVE_MS);
  }
});

function formatBytes(bytes) {
  return bytes < 1024 ? `${bytes} B` : `${(bytes / 1024).toFixed(1)} KB`;
}

function renderBackupFile(file) {
  const isLarge = file.size >= LOCAL_BACKUP_WARNING_BYTES;
  const modified = file.modified ? new Date(file.modified * 1000).toLocaleString() : 'Unknown date';
  const warning = isLarge
    ? '<small class="file-warning">Large backup: verify the NEO file limit before restoring.</small>'
    : '';
  return `<div class="action-row backup-row${isLarge ? ' is-large' : ''}"><span class="action-icon green">TXT</span><span><b>${escapeHtml(file.name)}</b><small>${formatBytes(file.size)} · ${escapeHtml(modified)}</small>${warning}</span><span class="table-actions"><button class="table-action" type="button" data-backup-action="view" data-name="${escapeHtml(file.name)}">View</button><button class="table-action" type="button" data-backup-action="send" data-name="${escapeHtml(file.name)}">Send text</button><button class="table-action danger-action" type="button" data-backup-action="delete" data-name="${escapeHtml(file.name)}">Delete</button></span></div>`;
}

document.querySelector('#upload-file')?.addEventListener('click', () => document.querySelector('#upload-file-input').click());

document.querySelector('#upload-file-input')?.addEventListener('change', async (event) => {
  const file = event.target.files?.[0];
  if (!file) return;
  try {
    const text = await file.text();
    const res = await authFetch(`/files?name=${encodeURIComponent(file.name)}`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain; charset=utf-8' },
      body: text,
    });
    if (!res.ok) throw new Error('Upload failed.');
    showNotice(`Uploaded ${file.name}.`, 'success');
    refreshFiles();
  } catch (error) {
    showNotice(error.message, 'error');
  } finally {
    event.target.value = '';
  }
});

document.querySelector('#file-list')?.addEventListener('click', async (event) => {
  const button = event.target.closest('[data-backup-action]');
  if (!button) return;
  const name = button.dataset.name;
  const action = button.dataset.backupAction;
  if (action === 'delete') {
    if (!confirm(`Delete "${name}" permanently? This cannot be undone.`)) return;
    try {
      const res = await authFetch(`/files?name=${encodeURIComponent(name)}&confirm=true`, { method: 'DELETE' });
      if (!res.ok) throw new Error('Delete failed.');
      showNotice('File deleted.', 'success');
      refreshFiles();
    } catch (error) {
      showNotice(error.message, 'error');
    }
    return;
  }
  if (action === 'send') {
    try {
      const res = await authFetch(`/files/view?name=${encodeURIComponent(name)}`);
      if (!res.ok) throw new Error('Could not read file.');
      const j = await res.json();
      document.querySelector('#dialog-label').textContent = 'BLUETOOTH';
      document.querySelector('#dialog-title').textContent = 'Send text';
      document.querySelector('#dialog-copy').textContent = `Send "${name}" as keystrokes to the paired host. Preview first, then confirm.`;
      document.querySelector('#dialog-actions-default').hidden = true;
      document.querySelector('#dialog-actions-pairing').hidden = true;
      document.querySelector('#dialog-actions-ble-send').hidden = false;
      document.querySelector('#ble-send-text').value = j.content || '';
      document.querySelector('#ble-preview-box').hidden = true;
      document.querySelector('#ble-send-status').textContent = '';
      blePreviewLength = 0;
      refreshBleStatus();
      dialog.showModal();
    } catch (error) {
      showNotice(error.message, 'error');
    }
    return;
  }
  if (action === 'view') {
    try {
      const res = await authFetch(`/files/view?name=${encodeURIComponent(name)}`);
      if (!res.ok) throw new Error('Could not read file.');
      const j = await res.json();
      activeBackupFile = name;
      document.querySelector('#file-action-title').textContent = name;
      document.querySelector('#file-action-preview').textContent = j.content || '';
      document.querySelector('#file-rename-input').value = name;
      document.querySelector('#file-action-dialog').showModal();
    } catch (error) {
      showNotice(error.message, 'error');
    }
  }
});

document.querySelector('#file-action-close')?.addEventListener('click', () => document.querySelector('#file-action-dialog').close());
document.querySelector('#file-download-btn')?.addEventListener('click', async () => {
  if (!activeBackupFile) return;
  try {
    const res = await authFetch(`/files/download?name=${encodeURIComponent(activeBackupFile)}`);
    if (!res.ok) throw new Error('Download failed.');
    const blob = await res.blob();
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = activeBackupFile;
    anchor.click();
    URL.revokeObjectURL(url);
  } catch (error) {
    showNotice(error.message, 'error');
  }
});

document.querySelector('#file-rename-btn')?.addEventListener('click', async () => {
  if (!activeBackupFile) return;
  const newName = document.querySelector('#file-rename-input').value.trim();
  if (!newName || newName === activeBackupFile) return;
  try {
    const res = await authFetch('/files', {
      method: 'PATCH',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ old_name: activeBackupFile, new_name: newName }),
    });
    if (!res.ok) throw new Error('Rename failed.');
    showNotice('File renamed.', 'success');
    document.querySelector('#file-action-dialog').close();
    refreshFiles();
  } catch (error) {
    showNotice(error.message, 'error');
  }
});

document.querySelector('#file-delete-btn')?.addEventListener('click', async () => {
  if (!activeBackupFile) return;
  if (!confirm(`Delete "${activeBackupFile}" permanently? This cannot be undone.`)) return;
  try {
    const res = await authFetch(`/files?name=${encodeURIComponent(activeBackupFile)}&confirm=true`, { method: 'DELETE' });
    if (!res.ok) throw new Error('Delete failed.');
    showNotice('File deleted.', 'success');
    document.querySelector('#file-action-dialog').close();
    refreshFiles();
  } catch (error) {
    showNotice(error.message, 'error');
  }
});
function escapeHtml(value) {
  return value.replace(/[&<>'"]/g, (character) => ({ '&':'&amp;', '<':'&lt;', '>':'&gt;', "'":'&#39;', '"':'&quot;' })[character]);
}

purgeExpiredLocalToken();
updateSignInState();
initHeaderFooter();

async function bootstrapAuthedUi() {
  await clearSessionIfOnboarding();
  if (!getAuthToken()) {
    updateSignInState();
    return;
  }
  const ok = await validateSession();
  updateSignInState();
  if (ok !== true) {
    /* false → cleared; null → device unreachable — stay on Sign In until proven */
    return;
  }
  await refreshStatus();
  if (IS_DASHBOARD) {
    refreshFiles();
    if (window.NEO2_PORTAL_DEMO) {
      refreshApplets();
      refreshNeoFiles();
    }
  }
  if (IS_TYPING_PAGE) refreshLiveText();
  if (!IS_NEO_LINK_PAGE) loadCloudSyncConfig();
}

if (getAuthToken()) {
  bootstrapAuthedUi();
} else {
  clearSessionIfOnboarding().then(() => updateSignInState());
}

/* Keep the server session alive and demote the UI if the token dies. */
setInterval(() => {
  if (document.hidden || !getAuthToken()) return;
  const expAt = Number(localStorage.getItem('neo2_token_exp_at') || 0);
  const soon = !expAt || Date.now() > expAt - 120000;
  if (soon || !sessionVerified) {
    validateSession().then((ok) => {
      updateSignInState();
      if (ok === true) refreshStatus().catch(() => {});
    });
  } else {
    refreshStatus().catch(() => {});
  }
}, 60000);
document.addEventListener('visibilitychange', () => {
  if (!document.hidden && getAuthToken()) {
    validateSession().then((ok) => {
      updateSignInState();
      if (ok === true) refreshStatus().catch(() => {});
    });
  }
});

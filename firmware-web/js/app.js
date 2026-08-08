const PORTAL_PAGE = document.body?.dataset?.page || 'dashboard';
const IS_TYPING_PAGE = PORTAL_PAGE === 'typing';
const IS_DASHBOARD = PORTAL_PAGE === 'dashboard';

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
  'open-pairing': ['BLUETOOTH', 'Pair keyboard', 'Make the buddy discoverable so a phone or computer can bond as a Bluetooth keyboard. Pairing for a new host stops after two minutes. Bonds are saved and reconnect after reboot. While connected, Neo keys are forwarded live over Bluetooth.'],
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
function setAuthToken(t) { if (t) localStorage.setItem('neo2_token', t); else localStorage.removeItem('neo2_token'); }

function authFetch(path, opts={}){
  opts.headers = opts.headers || {};
  const t = getAuthToken();
  if (!t) return Promise.reject(new Error('Authentication required'));
  opts.headers['Authorization'] = 'Bearer ' + t;
  return fetch(API_BASE + path, opts);
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
  throw new Error(message || `Request failed (${response.status})`);
}

function setButtonBusy(button, busy, busyLabel) {
  if (!button) return;
  if (busy) {
    button.dataset.label = button.textContent;
    button.textContent = busyLabel;
    button.disabled = true;
    button.setAttribute('aria-busy', 'true');
    return;
  }
  button.textContent = button.dataset.label || button.textContent;
  button.disabled = false;
  button.removeAttribute('aria-busy');
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

document.querySelector('#action-dialog-close')?.addEventListener('click', () => dialog.close());
document.querySelector('#action-dialog-form')?.addEventListener('submit', (event) => {
  if (event.submitter?.id !== 'action-dialog-close') return;
  event.preventDefault();
  dialog.close();
});

async function refreshBleStatus() {
  const line = document.querySelector('#ble-status-line');
  if (!line) return;
  try {
    const res = await authFetch('/ble');
    if (!res.ok) throw new Error('unavailable');
    const j = await res.json();
    const sendLine = document.querySelector('#ble-send-status');
    const stateLabel = j.state === 'connected'
      ? 'Connected — Neo keys pass through live'
      : j.state === 'pairing'
        ? 'Discoverable for pairing (2 min)'
        : (j.bonded > 0
          ? `Idle — ${j.bonded} bonded host${j.bonded === 1 ? '' : 's'}, waiting to reconnect`
          : 'Not paired');
    line.textContent = `${stateLabel}. ${j.can_send ? 'Host ready for Neo typing and portal Send text.' : 'Connect or pair a Bluetooth host to type.'}`;
    if (sBleConnected && sendLine) {
      sendLine.textContent = 'While Bluetooth is connected, ASM (manager) mode on the Documents page may stop keystrokes until keyboard mode returns.';
    } else if (sendLine) {
      sendLine.textContent = j.can_send
        ? 'Host ready. Neo keys pass through; portal text send is optional.'
        : 'Waiting for Bluetooth host connection...';
    }
  } catch (error) {
    line.textContent = 'Bluetooth status unavailable.';
  }
}

document.querySelector('#ble-start-pairing')?.addEventListener('click', async () => {
  try {
    const res = await authFetch('/ble/pairing', { method: 'POST', body: JSON.stringify({ enabled: true }), headers: { 'Content-Type': 'application/json' } });
    if (!res.ok) throw new Error('Pairing could not be started.');
    showNotice('Device is discoverable for 2 minutes.', 'success');
    refreshBleStatus();
  } catch (error) {
    showNotice(error.message, 'error');
  }
});

document.querySelector('#ble-stop-pairing')?.addEventListener('click', async () => {
  try {
    await authFetch('/ble/pairing', { method: 'POST', body: JSON.stringify({ enabled: false }), headers: { 'Content-Type': 'application/json' } });
    showNotice('Pairing stopped.', 'success');
    refreshBleStatus();
  } catch (error) {
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
  if (bucketField) bucketField.hidden = provider !== 's3';
  if (regionField) regionField.hidden = provider !== 's3';
  updateSyncProviderHelp(provider);
}

function updateSyncProviderHelp(provider) {
  const el = document.querySelector('#sync-provider-help');
  if (!el) return;
  if (provider === 's3') {
    el.innerHTML = '<strong>S3-compatible setup</strong><br>Use the bucket endpoint (for example Cloudflare R2 or AWS S3). Set <strong>Bucket</strong>, <strong>Region</strong> (<code>auto</code> for R2), access key ID as username, and secret access key. The buddy must be on home Wi‑Fi so the clock can sync before uploads.';
    return;
  }
  el.innerHTML = '<strong>WebDAV setup</strong><br>For Nextcloud, paste the full WebDAV folder URL (often ending in <code>/remote.php/dav/files/you/backups</code>). Use your account username and an app password — not your login password. The buddy creates the optional subfolder automatically before the first upload.';
}

function validateCloudSyncPayload(payload, credentialsConfigured) {
  if (!payload.endpoint || !payload.endpoint.startsWith('https://')) {
    throw new Error('Server URL must start with https://');
  }
  if (!payload.username) {
    throw new Error('Username or access key is required.');
  }
  if (payload.provider === 's3' && !payload.bucket) {
    throw new Error('Bucket name is required for S3-compatible storage.');
  }
  if (!credentialsConfigured && !payload.secret) {
    throw new Error('Enter an app password or secret key the first time you save.');
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
    else subtitle.textContent = 'WebDAV or S3 backup destination';
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
    const radio = document.querySelector(`input[name="provider"][value="${provider === 's3' ? 's3' : 'webdav'}"]`);
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
      if (ssidSelect) {
        ssidSelect.dataset.savedSsid = sj.wifi_ssid || '';
        ssidSelect.value = sj.wifi_ssid || '';
      }
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
  loginBtn.textContent = getAuthToken() ? 'Signed in' : 'Sign In';
  updateAuthVisibility();
}

function updateAuthVisibility() {
  const authed = !!getAuthToken();
  const logsBtn = document.getElementById('logs-button');
  const settingsBtn = document.getElementById('settings-button');
  if (logsBtn) logsBtn.hidden = !authed;
  if (settingsBtn) settingsBtn.hidden = !authed;
  // Admin-only controls
  const refreshFilesBtn = document.getElementById('refresh-files');
  const openSyncBtn = document.getElementById('open-sync');
  const syncRunBackupsBtn = document.getElementById('sync-run-backups');
  const refreshNeoBtn = document.getElementById('refresh-neo-files');
  const installAppletBtn = document.getElementById('install-applet');
  const refreshAppletsBtn = document.getElementById('refresh-applets');
  const neoCharmap = document.getElementById('neo-charmap');
  const neoReadAllBtn = document.getElementById('neo-read-all');
  const neoBackupNowBtn = document.getElementById('neo-backup-now');
  const neoRestartBtn = document.getElementById('neo-restart');
  const removeAllAppletsBtn = document.getElementById('remove-all-applets');
  if (refreshFilesBtn) refreshFilesBtn.hidden = !authed;
  const uploadFileBtn = document.getElementById('upload-file');
  if (uploadFileBtn) uploadFileBtn.hidden = !authed;
  if (openSyncBtn) openSyncBtn.hidden = !authed;
  if (syncRunBackupsBtn) syncRunBackupsBtn.hidden = !authed;
  if (refreshNeoBtn) refreshNeoBtn.hidden = !authed;
  if (neoCharmap) neoCharmap.hidden = !authed;
  if (neoReadAllBtn) neoReadAllBtn.hidden = !authed;
  if (neoBackupNowBtn) neoBackupNowBtn.hidden = !authed;
  if (neoRestartBtn) neoRestartBtn.hidden = !authed;
  if (installAppletBtn) installAppletBtn.hidden = !authed;
  if (refreshAppletsBtn) refreshAppletsBtn.hidden = !authed;
  if (removeAllAppletsBtn) removeAllAppletsBtn.hidden = !authed;
  const autoBackupWraps = ['dashboard-auto-backup-wrap', 'backups-auto-backup-wrap'];
  for (const id of autoBackupWraps) {
    const el = document.getElementById(id);
    if (el) el.hidden = !authed;
  }
}

// Login UI
const loginBtn = document.getElementById('login-btn');
const loginDialog = document.getElementById('login-dialog');
const loginForm = document.getElementById('login-form');
const loginCancel = document.getElementById('login-cancel');

loginBtn.addEventListener('click', () => {
  if (getAuthToken()) {
    setAuthToken(null);
    updateSignInState();
    document.querySelector('#connection').textContent = 'Not signed in';
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
    loginDialog.close();
    loginForm.reset();
    updateSignInState();
    refreshStatus();
    loadCloudSyncConfig();
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
      } else {
        const ssidSelect = document.querySelector('#wifi-ssid');
        const ssid = ssidSelect?.value.trim() || ssidSelect?.dataset.savedSsid || '';
        if (!ssid) {
          showNotice('Choose a home Wi‑Fi network (or scan and select one).', 'error');
          return;
        }
        body.wifi_ssid = ssid;
        const wifiPassword = document.querySelector('#wifi-password').value;
        if (wifiPassword) body.wifi_password = wifiPassword;
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
  try{
    // Prefer the consolidated /status path; fall back to legacy /usb/status.
    let res = await authFetch('/status').catch(() => authFetch('/usb/status'));
    if (!res.ok) {
      document.getElementById('connection').textContent = 'Not signed in';
      setNeoConnectionState({ usb_connected: false });
      return;
    }
    const j = await res.json();
    sBleState = j.ble_state || 'idle';
    sBleConnected = sBleState === 'connected';
    setNeoConnectionState(j);
    applyFeatureFlags(j);
    const fmtBtn = document.getElementById('format-sd');
    if (fmtBtn) fmtBtn.hidden = !j.have_sdcard;
    const conn = document.getElementById('connection');
    if (conn && j.ip) {
      const ble = j.ble_state === 'connected' ? 'Bluetooth connected' : j.ble_state === 'pairing' ? 'Bluetooth pairing' : 'Bluetooth idle';
      conn.textContent = j.ip ? `${j.ip} · ${ble}` : ble;
    }
    if (IS_DASHBOARD && sNeoKeyboardActive) {
      updateAppletCountFromCache();
    }
  }catch(e){
    document.getElementById('connection').textContent = 'Portal preview';
    setNeoConnectionState({ usb_connected: false });
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

document.addEventListener('click', (event) => {
  if (event.target?.id === 'portal-enable-autobackup') {
    event.preventDefault();
    saveAutoBackupOnConnect(true);
  }
});

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

function updatePortalNotices() {
  const el = document.getElementById('portal-notices');
  if (!el) return;
  let html = '';
  if (sBleConnected) {
    const docsHint = IS_DASHBOARD
      ? ' Use Scan, Backup, or Refresh only when you intend to leave keyboard mode.'
      : ' Open <a href="index.html">Documents</a> for file ops — those switch the Neo to ASM mode.';
    html =
      `<p class="portal-notice portal-notice-ble" role="status">` +
      `<strong>Bluetooth keyboard connected.</strong> ASM (manager) mode may <strong>stop Neo keystrokes</strong> on your paired device until keyboard mode returns.${docsHint}</p>`;
  } else if (IS_DASHBOARD && getAuthToken() && !sAutoBackupOnConnect) {
    html =
      '<p class="portal-notice portal-notice-info" role="status">' +
      '<strong>Tip:</strong> Enable <strong>Auto on connect</strong> on Documents or Backups to save changed files when you plug in the Neo ' +
      '(brief keyboard pause; returns automatically).' +
      ' <button type="button" class="portal-notice-action" id="portal-enable-autobackup">Enable now</button></p>';
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
  const ids = ['neo-backup-now', 'neo-read-all', 'refresh-neo-files', 'neo-restart', 'neo-rescan'];
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
  if (btn) btn.disabled = true;
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
    if (btn) btn.disabled = false;
  }
}

document.getElementById('neo-rescan')?.addEventListener('click', () => rescanNeo());

async function refreshFiles() {
  if (!filesEmpty || !fileList) return;
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
  if (opts.confirm && !confirmLeaveKeyboardMode('Refreshing applets')) return;
  try {
    const response = await authFetch('/command/list_applets');
    if (!response.ok) return;
    const applets = await response.json();
    if (appletsEmpty) appletsEmpty.hidden = applets.length > 0;
    if (appletList) {
      appletList.hidden = applets.length === 0;
      appletList.innerHTML = applets.map((applet) => renderApplet(applet)).join('');
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
  }
}

async function refreshNeoFiles(opts = {}) {
  const button = document.querySelector('#refresh-neo-files');
  const guidance = document.querySelector('#neo-documents-guidance');
  if (!getAuthToken()) {
    guidance.textContent = 'Sign in to scan documents on the connected NEO.';
    return;
  }
  if (opts.confirm && !confirmLeaveKeyboardMode('Scanning Neo documents')) return;
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
    renderNeoFiles(alphaWordFiles);
    guidance.textContent = alphaWordFiles.length
      ? `${alphaWordFiles.length} AlphaWord document${alphaWordFiles.length === 1 ? '' : 's'} on the connected NEO.`
      : 'No AlphaWord documents were reported by the connected NEO.';
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
  neoFileList.innerHTML = files.map((file) => {
    const allocatedSize = Number(file.alloc_size || file.allocated_size || 0);
    const isLarge = allocatedSize >= LOCAL_BACKUP_WARNING_BYTES;
    const fileIndex = Number(file.file_index || file.index || 0);
    const appletId = NEO_ALPHAWORD_ID;
    return `<tr${isLarge ? ' class="is-large"' : ''}><td><strong>${escapeHtml(file.name || 'Untitled')}</strong>${isLarge ? '<small class="file-warning">Review before restoring</small>' : ''}</td><td>${fileIndex || '—'}</td><td>${formatBytes(allocatedSize)}</td><td class="table-actions"><button class="table-action" type="button" data-file-action="read" data-applet-id="${appletId}" data-file-index="${fileIndex}" data-file-name="${escapeHtml(file.name || '')}">Read</button><button class="table-action" type="button" data-file-action="download" data-applet-id="${appletId}" data-file-index="${fileIndex}">Download</button><button class="table-action" type="button" data-file-action="write" data-applet-id="${appletId}" data-file-index="${fileIndex}" data-file-name="${escapeHtml(file.name || '')}">Write</button><button class="table-action danger-action" type="button" data-file-action="clear" data-applet-id="${appletId}" data-file-index="${fileIndex}" data-file-name="${escapeHtml(file.name || '')}">Clear</button></td></tr>`;
  }).join('');
}

function renderApplet(applet) {
  const id = Number(applet.id);
  const title = escapeHtml(applet.name || `Applet ${id}`);
  const details = `ID ${id} · ${applet.file_count} file${applet.file_count === 1 ? '' : 's'} · ${formatBytes(applet.rom_size)} ROM`;
  return `<div class="action-row applet-row"><span class="action-icon blue">APP</span><span><b>${title}</b><small>${details}</small></span><span class="applet-actions"><button class="table-action" type="button" data-applet-action="download" data-applet-id="${id}" data-applet-name="${title}">Download</button><button class="table-action danger-action" type="button" data-applet-action="remove" data-applet-id="${id}" data-applet-name="${title}">Remove</button></span></div>`;
}

function openAppletDialog(operation, details = {}) {
  appletDialogOperation = operation;
  const install = operation === 'install';
  document.querySelector('#applet-dialog-label').textContent = install ? 'SMARTAPPLET INSTALL' : 'SMARTAPPLET REMOVAL';
  document.querySelector('#applet-dialog-title').textContent = install ? 'Install SmartApplet' : `Remove ${details.name || 'SmartApplet'}?`;
  document.querySelector('#applet-install-fields').hidden = !install;
  document.querySelector('#applet-submit').textContent = install ? 'Install applet' : 'Remove applet';
  document.querySelector('#applet-dialog-copy').textContent = install
    ? 'The NEO validates the package header and checks available ROM and RAM before installation.'
    : 'This permanently removes the selected SmartApplet and its files from the NEO. This cannot be undone.';
  document.querySelector('#applet-dialog-status').textContent = '';
  appletDialog.dataset.appletId = details.id || '';
  appletDialog.showModal();
}

async function invokeFileAction(action, appletId, fileIndex, fileName = '') {
  if (action === 'clear') {
    if (!window.confirm(`Clear "${fileName || `file ${fileIndex}`}" on the NEO? This cannot be undone.`)) return;
    if (!confirmLeaveKeyboardMode('Clearing a Neo file')) return;
    try {
      await apiRequest(`/neo/applets/${appletId}/files/${fileIndex}`, { method: 'DELETE' });
      showNotice('Document cleared on the NEO.', 'success');
      await refreshNeoFiles();
    } catch (error) {
      showNotice(`Clear failed: ${error.message}`, 'error');
    }
    return;
  }
  if (action === 'write') {
    if (!confirmLeaveKeyboardMode('Writing to a Neo file')) return;
    activeWriteTarget = { appletId: Number(appletId), fileIndex: Number(fileIndex), fileName };
    document.querySelector('#write-document-title').textContent = fileName || `File ${fileIndex}`;
    document.querySelector('#write-document-meta').textContent = `Applet ${appletId} · file ${fileIndex}`;
    document.querySelector('#write-document-content').value = '';
    document.querySelector('#write-document-status').textContent = '';
    writeDocumentDialog.showModal();
    return;
  }
  if ((action === 'read' || action === 'download') &&
      !confirmLeaveKeyboardMode(action === 'read' ? 'Reading a Neo file' : 'Downloading a Neo file')) {
    return;
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
document.querySelector('#refresh-neo-files')?.addEventListener('click', () => refreshNeoFiles({ confirm: true }));
document.querySelector('#neo-read-all')?.addEventListener('click', backupAllNeoFiles);
document.querySelector('#neo-backup-now')?.addEventListener('click', backupNowNeoFiles);
document.querySelector('#neo-restart')?.addEventListener('click', restartNeo);
document.querySelector('#remove-all-applets')?.addEventListener('click', removeAllApplets);
document.querySelector('#install-applet')?.addEventListener('click', () => openAppletDialog('install'));
document.querySelector('#applet-close')?.addEventListener('click', () => appletDialog.close());
document.querySelector('#applet-cancel')?.addEventListener('click', () => appletDialog.close());
document.querySelector('#applet-file')?.addEventListener('change', async (event) => {
  const file = event.target.files[0];
  document.querySelector('#applet-file-name').textContent = file?.name || 'Choose a package file';
  const status = document.querySelector('#applet-dialog-status');
  if (!file || appletDialogOperation !== 'install' || !getAuthToken()) return;
  status.textContent = 'Inspecting package header…';
  try {
    const response = await apiRequest('/neo/applets/inspect', {
      method: 'POST',
      body: file,
      headers: { 'Content-Type': 'application/octet-stream' }
    });
    const info = await response.json();
    status.textContent = `${info.name} · ID ${info.applet_id} · ${formatBytes(info.rom_size)} ROM · ${info.file_count} file${info.file_count === 1 ? '' : 's'}`;
  } catch (error) {
    status.textContent = error.message || 'Could not inspect the package.';
  }
});
document.querySelector('#write-document-close')?.addEventListener('click', () => writeDocumentDialog.close());
document.querySelector('#write-document-cancel')?.addEventListener('click', () => writeDocumentDialog.close());
writeDocumentForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!activeWriteTarget) return;
  const status = document.querySelector('#write-document-status');
  const text = document.querySelector('#write-document-content').value;
  status.textContent = 'Writing to the NEO…';
  try {
    await apiRequest(
      `/neo/applets/${activeWriteTarget.appletId}/files/${activeWriteTarget.fileIndex}/write${neoCharmapQuery('?')}`,
      { method: 'POST', body: text, headers: { 'Content-Type': 'text/plain; charset=utf-8' } }
    );
    writeDocumentDialog.close();
    showNotice('Document written to the NEO.', 'success');
    await refreshNeoFiles();
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
  if (button.dataset.appletAction === 'remove') openAppletDialog('remove', { id: button.dataset.appletId, name: button.dataset.appletName });
  if (button.dataset.appletAction === 'download') invokeAppletDownload(button.dataset.appletId, button.dataset.appletName);
});
appletForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  const leaveLabel = appletDialogOperation === 'install' ? 'Installing a SmartApplet' : 'Removing a SmartApplet';
  if (!confirmLeaveKeyboardMode(leaveLabel)) return;
  const status = document.querySelector('#applet-dialog-status');
  const submitButton = document.querySelector('#applet-submit');
  status.textContent = 'Contacting the NEO...';
  setButtonBusy(submitButton, true, appletDialogOperation === 'install' ? 'Installing...' : 'Removing...');
  try {
    if (appletDialogOperation === 'install') {
      const file = document.querySelector('#applet-file').files[0];
      if (!file) throw new Error('Choose a SmartApplet package first.');
      await apiRequest('/neo/applets', { method: 'POST', body: file, headers: { 'Content-Type': 'application/octet-stream', 'X-Neo-Replace': String(document.querySelector('#applet-replace').checked) } });
    } else {
      await apiRequest(`/neo/applets/${appletDialog.dataset.appletId}`, { method: 'DELETE' });
    }
    appletDialog.close();
    refreshApplets();
  } catch (error) {
    status.textContent = error.message || 'The operation could not be completed.';
  } finally {
    setButtonBusy(submitButton, false);
  }
});

async function invokeAppletDownload(appletId, appletName) {
  if (!confirmLeaveKeyboardMode('Downloading a SmartApplet')) return;
  try {
    const response = await apiRequest(`/neo/applets/${appletId}/download`);
    const blob = await response.blob();
    const downloadUrl = URL.createObjectURL(blob);
    const link = Object.assign(document.createElement('a'), { href: downloadUrl, download: `${appletName || `applet-${appletId}`}.os3kapp` });
    link.click();
    URL.revokeObjectURL(downloadUrl);
  } catch (error) {
    document.querySelector('#applet-count-detail').textContent = `Applet download unavailable: ${error.message || 'USB transport is not ready.'}`;
  }
}

async function backupAllNeoFiles() {
  const button = document.querySelector('#neo-read-all');
  if (sBackupBusy) {
    showNotice('A backup is already running.', 'error');
    return;
  }
  if (!confirmLeaveKeyboardMode('Backup all')) return;
  setButtonBusy(button, true, 'Backing up…');
  try {
    const response = await apiRequest(`/neo/applets/${NEO_ALPHAWORD_ID}/files/read-all${neoCharmapQuery('?')}`, { method: 'POST' });
    const result = await response.json();
    const kb = result.returned_to_keyboard ? ' Neo returned to keyboard mode.' : ' Could not return Neo to keyboard — use Keyboard mode.';
    showNotice(`Backed up ${result.count || 0} AlphaWord document${result.count === 1 ? '' : 's'} locally.${kb}`, result.returned_to_keyboard ? 'success' : 'error');
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
  if (!confirmLeaveKeyboardMode('Backup now')) return;
  setButtonBusy(button, true, 'Starting…');
  try {
    await apiRequest('/neo/autobackup', { method: 'POST' });
    showNotice('Backup started — changed AlphaWord files only, then keyboard mode.', 'success');
    sAwaitingBackupFinish = true;
    setBackupBusyUi(true);
    await refreshStatus();
  } catch (error) {
    showNotice(`Backup now failed: ${error.message}`, 'error');
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
  if (!confirmLeaveKeyboardMode('Removing all SmartApplets')) return;
  if (!window.confirm('Remove ALL SmartApplets from the NEO? The device will reboot and this cannot be undone.')) return;
  const button = document.querySelector('#remove-all-applets');
  setButtonBusy(button, true, 'Removing…');
  try {
    await apiRequest('/neo/applets', { method: 'DELETE' });
    showNotice('All SmartApplets removed from the NEO.', 'success');
    await refreshApplets();
    await refreshNeoFiles();
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
  sel.innerHTML = '<option value="">(scanning...)</option>';
  try {
    const res = await authFetch('/wifi/scan');
    if (!res.ok) throw new Error('scan failed');
    const arr = await res.json();
    const options = ['<option value="">(select a network)</option>'];
    const seen = new Set();
    if (saved) {
      options.push(`<option value="${escapeHtml(saved)}">${escapeHtml(saved)} (saved)</option>`);
      seen.add(saved);
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
    sel.innerHTML = saved
      ? `<option value="">(scan failed)</option><option value="${escapeHtml(saved)}">${escapeHtml(saved)} (saved)</option>`
      : '<option value="">(scan failed)</option>';
    if (saved) sel.value = saved;
  }
}

document.querySelector('#wifi-scan-btn')?.addEventListener('click', () => fetchWifiScan());

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

setInterval(() => {
  if (!document.hidden) refreshStatus();
}, 12000);
if (IS_TYPING_PAGE) {
  setInterval(() => {
    if (!document.hidden) refreshLiveText();
  }, 1000);
}
document.addEventListener('visibilitychange', () => {
  if (!document.hidden) {
    refreshStatus();
    if (IS_TYPING_PAGE) refreshLiveText();
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

refreshStatus();
if (IS_DASHBOARD) {
  refreshFiles();
  if (window.NEO2_PORTAL_DEMO) {
    refreshApplets();
    refreshNeoFiles();
  }
}
if (IS_TYPING_PAGE) refreshLiveText();
updateSignInState();
loadCloudSyncConfig();
initHeaderFooter();

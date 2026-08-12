/**
 * Shared portal core — auth, header, status chip, logs, settings entry.
 * Loaded on every interactive page. Page modules: app.js / neo-link.js / typing extras.
 */
(function (global) {
  'use strict';

  const PORTAL_PAGE = document.body?.dataset?.page || 'dashboard';
  const API_BASE = '/api/v1';

  function getAuthToken() {
    return localStorage.getItem('neo2_token');
  }

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

  function rememberTokenExpiry(expiresInSec) {
    const sec = Number(expiresInSec);
    if (!Number.isFinite(sec) || sec <= 0) {
      localStorage.removeItem('neo2_token_exp_at');
      return;
    }
    localStorage.setItem('neo2_token_exp_at', String(Date.now() + sec * 1000));
  }

  function invalidateSession(notice) {
    const had = !!getAuthToken() || sessionVerified;
    sessionVerified = false;
    setAuthToken(null);
    if (had) {
      updateSignInState();
      if (notice) showNotice(notice, 'error');
    }
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

  /** Parse JSON bodies; map plain-text 401 "unauthorized" to a clear sign-in error. */
  async function readJson(res) {
    const text = await res.text();
    if (res.status === 401) {
      // authFetch already cleared the session when used; still safe if called alone.
      if (getAuthToken()) invalidateSession('Session expired — sign in again');
      throw new Error('Session expired — sign in again');
    }
    if (!text) {
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      return {};
    }
    try {
      return JSON.parse(text);
    } catch (_) {
      if (!res.ok) throw new Error(text.slice(0, 160) || `HTTP ${res.status}`);
      throw new Error('Invalid JSON from device');
    }
  }

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

  function showNotice(message, type = 'info') {
    const el = document.getElementById('portal-notices');
    if (!el) {
      if (type === 'error') console.warn(message);
      return;
    }
    el.hidden = false;
    el.innerHTML =
      `<p class="portal-notice portal-notice-${type === 'error' ? 'info' : type === 'success' ? 'info' : 'info'}" role="status">${escapeHtml(message)}</p>`;
  }

  function escapeHtml(value) {
    return String(value).replace(/[&<>'"]/g, (c) =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;' }[c])
    );
  }

  function initHeaderFooter() {
    const appMeta =
      document.querySelector('meta[name="application-name"]')?.getAttribute('content') || 'Neo2 Buddy Portal';
    const appVer = document.querySelector('meta[name="application-version"]')?.getAttribute('content') || '';
    const appNameEl = document.getElementById('app-name');
    const headerDeviceEl = document.getElementById('header-device-name');
    const footerApp = document.getElementById('footer-app-name');
    const footerVer = document.getElementById('footer-version');
    if (appNameEl) appNameEl.textContent = appMeta;
    if (footerApp) footerApp.textContent = appMeta;
    if (footerVer && appVer) footerVer.textContent = 'v' + appVer;
    if (headerDeviceEl) {
      headerDeviceEl.textContent = localStorage.getItem('neo2_device_name') || 'Neo2 Buddy';
    }
  }

  function clearGuestUi() {
    const conn = document.getElementById('connection');
    if (conn) conn.textContent = 'Not signed in';
    const neoConn = document.getElementById('neo-connection');
    if (neoConn) neoConn.textContent = 'Sign in';
    const neoDetail = document.getElementById('neo-connection-detail');
    if (neoDetail) neoDetail.textContent = 'Sign in to see Neo USB status.';
    const model = document.getElementById('neo-model');
    if (model) model.textContent = '—';
    const modelDetail = document.getElementById('neo-model-detail');
    if (modelDetail) modelDetail.textContent = 'Available after sign-in';
    const liveText = document.getElementById('live-text');
    if (liveText) liveText.textContent = 'Sign in to monitor live Neo keyboard input.';
    const notices = document.getElementById('portal-notices');
    if (notices) {
      notices.hidden = true;
      notices.innerHTML = '';
    }
  }

  function updateSignInState() {
    const loginBtn = document.getElementById('login-btn');
    const authed = isSessionAuthed();
    if (loginBtn) {
      loginBtn.textContent = authed ? 'Sign out' : 'Sign In';
      loginBtn.title = authed ? 'Sign out of the portal' : 'Sign in to manage this device';
      loginBtn.setAttribute('aria-pressed', authed ? 'true' : 'false');
    }
    document.body.classList.toggle('portal-authed', authed);
    document.body.classList.toggle('portal-guest', !authed);
    ['logs-button', 'settings-button'].forEach((id) => {
      const el = document.getElementById(id);
      if (el) el.hidden = !authed;
    });
    document.querySelectorAll('[data-auth-required]').forEach((el) => {
      el.hidden = !authed;
    });
    if (!authed) {
      clearGuestUi();
    } else {
      const conn = document.getElementById('connection');
      if (conn && (!conn.textContent || conn.textContent === 'Not signed in')) {
        conn.textContent = 'Signed in…';
      }
    }
    if (typeof global.Neo2?.onAuthChange === 'function') {
      global.Neo2.onAuthChange(authed);
    }
  }

  async function refreshStatus() {
    const conn = document.getElementById('connection');
    if (!isSessionAuthed()) {
      if (conn) conn.textContent = 'Not signed in';
      return;
    }
    try {
      const res = await authFetch('/status');
      if (!res.ok) {
        if (res.status === 401) return;
        if (conn) conn.textContent = 'Signed in · status unavailable';
        return;
      }
      const j = await res.json();
      if (conn) {
        const ble =
          j.ble_state === 'connected'
            ? 'Bluetooth connected'
            : j.ble_state === 'pairing'
              ? 'Bluetooth pairing'
              : 'Bluetooth idle';
        conn.textContent = j.ip ? `${j.ip} · ${ble}` : ble;
      }
      if (typeof global.Neo2?.onStatus === 'function') global.Neo2.onStatus(j);
    } catch (e) {
      if (!isSessionAuthed()) return;
      if (conn) conn.textContent = 'Signed in · reconnecting…';
    }
  }

  /* ---- login ---- */
  const loginBtn = document.getElementById('login-btn');
  const loginDialog = document.getElementById('login-dialog');
  const loginForm = document.getElementById('login-form');
  loginBtn?.addEventListener('click', () => {
    if (isSessionAuthed()) {
      invalidateSession();
      return;
    }
    const err = document.querySelector('#login-error');
    if (err) err.hidden = true;
    loginDialog?.showModal();
    document.querySelector('#login-password')?.focus();
  });
  document.getElementById('login-cancel')?.addEventListener('click', () => loginDialog?.close());
  loginForm?.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const pw = document.getElementById('login-password')?.value || '';
    try {
      const res = await fetch(API_BASE + '/login', {
        method: 'POST',
        body: JSON.stringify({ password: pw }),
        headers: { 'Content-Type': 'application/json' },
      });
      if (!res.ok) throw new Error('The password was not accepted.');
      const j = await res.json();
      setAuthToken(j.token);
      rememberTokenExpiry(j.expires_in);
      sessionVerified = true;
      loginDialog?.close();
      loginForm.reset();
      updateSignInState();
      await refreshStatus();
      if (typeof global.Neo2?.onLogin === 'function') global.Neo2.onLogin();
      showNotice('Signed in.', 'success');
    } catch (error) {
      const err = document.querySelector('#login-error');
      if (err) {
        err.textContent =
          error.message === 'The password was not accepted.'
            ? error.message
            : 'The portal could not reach the device. Check the local connection and try again.';
        err.hidden = false;
      }
    }
  });

  /* ---- logs (minimal) ---- */
  const logsDialog = document.getElementById('logs-dialog');
  const logsList = document.getElementById('logs-list');
  document.getElementById('logs-button')?.addEventListener('click', async () => {
    if (!logsDialog) return;
    logsDialog.showModal();
    if (!logsList || !getAuthToken()) return;
    try {
      const res = await authFetch('/logs?limit=50');
      if (!res.ok) throw new Error('logs');
      const rows = await res.json();
      if (!Array.isArray(rows) || !rows.length) {
        logsList.innerHTML = '<p class="logs-empty">No logs yet</p>';
        return;
      }
      logsList.innerHTML = rows
        .map((r) => {
          const lvl = (r.level || 'INFO').toLowerCase();
          return `<div class="log-row"><div class="log-row-head"><span class="log-badge log-badge-${escapeHtml(lvl)}">${escapeHtml((r.level || 'INFO').toUpperCase())}</span></div><pre class="log-msg">${escapeHtml(r.msg || '')}</pre></div>`;
        })
        .join('');
    } catch (e) {
      logsList.innerHTML = '<p class="logs-empty">Could not load logs</p>';
    }
  });
  document.getElementById('logs-close')?.addEventListener('click', () => logsDialog?.close());

  /* ---- settings button: open full dialog when present (Documents / Typing) ---- */
  document.querySelectorAll('[data-action="settings"], [data-action="wifi"]').forEach((btn) => {
    btn.addEventListener('click', () => {
      if (typeof global.Neo2?.openSettings === 'function') {
        global.Neo2.openSettings({ focusNetwork: btn.dataset.action === 'wifi' });
      } else {
        document.getElementById('settings-dialog')?.showModal();
      }
    });
  });
  document.getElementById('settings-close')?.addEventListener('click', () => {
    document.getElementById('settings-dialog')?.close();
  });
  document.getElementById('settings-close-2')?.addEventListener('click', () => {
    document.getElementById('settings-dialog')?.close();
  });

  global.Neo2 = Object.assign(global.Neo2 || {}, {
    fromCore: true,
    PORTAL_PAGE,
    API_BASE,
    getAuthToken,
    setAuthToken,
    isSessionAuthed,
    authFetch,
    readJson,
    showNotice,
    escapeHtml,
    refreshStatus,
    initHeaderFooter,
    updateSignInState,
    invalidateSession,
    validateSession,
  });

  initHeaderFooter();
  purgeExpiredLocalToken();
  updateSignInState();
  (async () => {
    await clearSessionIfOnboarding();
    if (!getAuthToken()) {
      updateSignInState();
      return;
    }
    const ok = await validateSession();
    updateSignInState();
    if (ok === true) refreshStatus().catch(() => {});
  })();
  /* Typing page: no periodic /status — only load/login + disconnect recovery via typing.js */
  if (PORTAL_PAGE !== 'typing') {
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
    }, PORTAL_PAGE === 'neo-link' ? 30000 : 15000);
  } else {
    setInterval(() => {
      if (document.hidden || !getAuthToken()) return;
      validateSession().then(() => updateSignInState());
    }, 60000);
  }
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && getAuthToken()) {
      validateSession().then(() => updateSignInState());
    }
  });
})(window);

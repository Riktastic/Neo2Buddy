/**
 * Device settings dialog — shared by Documents / Typing.
 * Depends on js/core.js (window.Neo2).
 */
(function () {
  'use strict';
  if (!window.Neo2) return;

  const authFetch = (path, opts) => Neo2.authFetch(path, opts);
  const getAuthToken = () => Neo2.getAuthToken();
  const showNotice = (msg, type) => Neo2.showNotice(msg, type);
  const escapeHtml = (v) => Neo2.escapeHtml(v);
  const settingsDialog = document.getElementById('settings-dialog');
  if (!settingsDialog) return;

  /** @type {{ssid: string, password_set?: boolean, preferred?: boolean, password?: string}[]} */
  let savedWifiNetworks = [];

  async function apiRequest(path, options = {}) {
    const res = await authFetch(path, options);
    if (!res.ok) {
      let err = 'Request failed';
      try {
        const j = await res.json();
        if (j.error) err = j.error;
      } catch (_) {}
      throw new Error(err);
    }
    return res;
  }

  function portalSettings() {
    try {
      return JSON.parse(localStorage.getItem('neo2_portal_settings') || '{}');
    } catch (_) {
      return {};
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

  function toggleStaticIpFields() {
    const dhcpEl = document.querySelector('#wifi-dhcp');
    const staticWrap = document.querySelector('#static-ip-fields');
    if (!dhcpEl || !staticWrap) return;
    if (dhcpEl.checked) {
      staticWrap.hidden = true;
      staticWrap.querySelectorAll('input').forEach((i) => (i.disabled = true));
    } else {
      staticWrap.hidden = false;
      staticWrap.querySelectorAll('input').forEach((i) => (i.disabled = false));
      const note = document.querySelector('#settings-note');
      if (note) note.textContent = 'Warning: applying a static IP may make the device inaccessible. Proceed with caution.';
    }
  }

  function renderSavedWifiList() {
    const list = document.getElementById('wifi-saved-list');
    if (!list) return;
    if (!savedWifiNetworks.length) {
      list.innerHTML =
        '<li class="wifi-saved-empty">No saved home networks yet. Scan, enter the password, then Add / update.</li>';
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
    } catch (_) {
      const opts = ['<option value="">(scan failed)</option>'];
      for (const ssid of savedSet) {
        opts.push(`<option value="${escapeHtml(ssid)}">${escapeHtml(ssid)} (saved)</option>`);
      }
      sel.innerHTML = opts.join('');
      if (saved) sel.value = saved;
    }
  }

  async function openSettings(options = {}) {
    const settings = portalSettings();
    const deviceName = settings.deviceName || localStorage.getItem('neo2_device_name') || 'Neo2 Buddy';
    const nameEl = document.querySelector('#settings-device-name');
    const titleEl = document.querySelector('#settings-device-title');
    if (nameEl) nameEl.value = deviceName;
    if (titleEl) titleEl.textContent = deviceName;
    const sleepEl = document.querySelector('#settings-sleep');
    if (sleepEl) sleepEl.value = settings.sleep || '10';
    const priv = document.querySelector('#settings-private-live');
    if (priv) priv.checked = settings.privateLive !== false;
    const prefer = document.querySelector('#settings-prefer-sd');
    if (prefer) prefer.checked = settings.preferSd !== false;
    toggleStaticIpFields();
    const note = document.querySelector('#settings-note');
    if (note) note.textContent = 'Loading device settings…';
    try {
      const res = await authFetch('/settings');
      if (res.ok) {
        const sj = await res.json();
        const reqAuth = document.querySelector('#settings-require-auth');
        if (reqAuth) reqAuth.checked = !!sj.require_portal_auth;
        const autoBak = document.querySelector('#settings-auto-backup');
        if (autoBak) autoBak.checked = !!sj.auto_backup_on_connect;
        const autoCloud = document.querySelector('#settings-auto-cloud');
        if (autoCloud) autoCloud.checked = !!sj.auto_cloud_sync_after_backup;
        const neoLabel = document.querySelector('#settings-neo-label');
        if (neoLabel) neoLabel.value = sj.neo_label || '';
        if (sj.device_name && nameEl) {
          nameEl.value = sj.device_name;
          if (titleEl) titleEl.textContent = sj.device_name;
        }
        const layout = document.querySelector('#settings-keyboard-layout');
        if (layout && sj.keyboard_layout) layout.value = sj.keyboard_layout;
        if (sleepEl && sj.sleep_timeout_seconds) sleepEl.value = String(sj.sleep_timeout_seconds);
        const mode = sj.network_mode === 'home' ? 'home' : 'direct';
        const modeInput = document.querySelector(`input[name="settings-network-mode"][value="${mode}"]`);
        if (modeInput) modeInput.checked = true;
        const hs = document.querySelector('#hotspot-ssid');
        if (hs) hs.value = sj.hotspot_ssid || '';
        const hp = document.querySelector('#hotspot-password');
        if (hp) hp.value = '';
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
        const wp = document.querySelector('#wifi-password');
        if (wp) wp.value = '';
        const dhcpEl = document.querySelector('#wifi-dhcp');
        if (dhcpEl) dhcpEl.checked = sj.wifi_dhcp !== false;
        const map = {
          'wifi-ip': sj.wifi_ip,
          'wifi-netmask': sj.wifi_netmask,
          'wifi-gateway': sj.wifi_gateway,
          'wifi-dns': sj.wifi_dns,
        };
        Object.keys(map).forEach((id) => {
          const el = document.querySelector('#' + id);
          if (el) el.value = map[id] || '';
        });
        toggleStaticIpFields();
        if (note) note.textContent = 'Changes to network mode or Wi‑Fi settings restart the buddy in about two seconds.';
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
          } catch (_) {}
        }
      } else if (note) {
        note.textContent = getAuthToken()
          ? 'Could not load device settings. Network changes may not apply.'
          : 'Sign in to load and save device settings.';
      }
    } catch (_) {
      if (note) note.textContent = 'Could not reach the device settings API.';
    }
    syncSettingsNetworkModeUi();
    if (getAuthToken()) fetchWifiScan();
    settingsDialog.showModal();
    if (options.focusNetwork) {
      const section = document.getElementById('wifi-section');
      if (section) requestAnimationFrame(() => section.scrollIntoView({ behavior: 'smooth', block: 'start' }));
    }
  }

  document.querySelector('#settings-close')?.addEventListener('click', () => settingsDialog.close());
  document.querySelector('#settings-close-2')?.addEventListener('click', () => settingsDialog.close());
  document.querySelector('#wifi-dhcp')?.addEventListener('change', toggleStaticIpFields);
  document.querySelectorAll('input[name="settings-network-mode"]').forEach((input) => {
    input.addEventListener('change', syncSettingsNetworkModeUi);
  });
  document.querySelector('#settings-reset')?.addEventListener('click', () => {
    localStorage.removeItem('neo2_portal_settings');
    openSettings();
  });
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

  document.querySelector('#settings-form')?.addEventListener('submit', (event) => {
    event.preventDefault();
    const settings = {
      deviceName: document.querySelector('#settings-device-name')?.value.trim() || 'Neo2 Buddy',
      sleep: document.querySelector('#settings-sleep')?.value || '10',
      privateLive: !!document.querySelector('#settings-private-live')?.checked,
      preferSd: !!document.querySelector('#settings-prefer-sd')?.checked,
    };
    localStorage.setItem('neo2_device_name', settings.deviceName);
    localStorage.setItem('neo2_portal_settings', JSON.stringify(settings));
    const titleEl = document.querySelector('#settings-device-title');
    if (titleEl) titleEl.textContent = settings.deviceName;
    if (document.body.dataset.page === 'dashboard') {
      const headerDeviceEl = document.getElementById('header-device-name');
      if (headerDeviceEl) headerDeviceEl.textContent = settings.deviceName;
    }
    (async () => {
      const note = document.querySelector('#settings-note');
      if (!getAuthToken()) {
        if (note) note.textContent = 'Saved locally. Sign in to apply settings to the buddy.';
        return;
      }
      try {
        const mode = settingsNetworkMode();
        const body = {
          device_name: settings.deviceName,
          sleep_timeout_seconds: Number(settings.sleep),
          keyboard_layout: document.querySelector('#settings-keyboard-layout')?.value,
          require_portal_auth: !!document.querySelector('#settings-require-auth')?.checked,
          auto_backup_on_connect: !!document.querySelector('#settings-auto-backup')?.checked,
          auto_cloud_sync_after_backup: !!document.querySelector('#settings-auto-cloud')?.checked,
          neo_label: (document.querySelector('#settings-neo-label')?.value || '').trim(),
          network_mode: mode,
          hotspot_ssid: document.querySelector('#hotspot-ssid')?.value.trim(),
          wifi_dhcp: !!document.querySelector('#wifi-dhcp')?.checked,
          wifi_ip: document.querySelector('#wifi-ip')?.value.trim(),
          wifi_netmask: document.querySelector('#wifi-netmask')?.value.trim(),
          wifi_gateway: document.querySelector('#wifi-gateway')?.value.trim(),
          wifi_dns: document.querySelector('#wifi-dns')?.value.trim(),
        };
        const hotspotPassword = document.querySelector('#hotspot-password')?.value || '';
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
            const wifiPassword = document.querySelector('#wifi-password')?.value || '';
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
        const res = await apiRequest('/settings', {
          method: 'POST',
          body: JSON.stringify(body),
          headers: { 'Content-Type': 'application/json' },
        });
        const result = await res.json().catch(() => ({}));
        if (result.rebooting) {
          showNotice('The buddy is restarting…', 'success');
          if (note) {
            note.textContent =
              mode === 'home'
                ? 'Network settings saved. Rejoin your home Wi‑Fi in about a minute, then open the portal at the buddy’s new IP.'
                : 'Network settings saved. Rejoin the buddy hotspot in about a minute, then open http://192.168.4.1/';
          }
        } else {
          showNotice('Device settings saved.', 'success');
          if (note) note.textContent = 'Settings saved.';
        }
      } catch (e) {
        showNotice('Failed to save device settings: ' + (e.message || ''), 'error');
      }
    })();
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

  document.getElementById('factory-reset-open')?.addEventListener('click', (ev) => {
    ev.preventDefault();
    const dlg = document.getElementById('factory-reset-dialog');
    const pw = document.getElementById('factory-reset-password');
    if (pw) pw.value = '';
    if (dlg?.showModal) dlg.showModal();
  });
  const factoryResetForm = document.getElementById('factory-reset-form');
  if (factoryResetForm) {
    const closeFactoryReset = () => {
      document.getElementById('factory-reset-dialog')?.close();
      const pw = document.getElementById('factory-reset-password');
      if (pw) pw.value = '';
    };
    ['factory-reset-cancel', 'factory-reset-cancel-2'].forEach((id) => {
      document.getElementById(id)?.addEventListener('click', closeFactoryReset);
    });
    factoryResetForm.addEventListener('submit', (ev) => {
      ev.preventDefault();
      const password = document.getElementById('factory-reset-password')?.value || '';
      if (!password) {
        showNotice('Enter the portal password to confirm.', 'error');
        return;
      }
      (async () => {
        try {
          const res = await authFetch('/device/factory-reset', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ password }),
          });
          if (!res.ok) throw new Error('Factory reset failed.');
          showNotice('Factory reset started. The buddy will reboot.', 'success');
          closeFactoryReset();
        } catch (e) {
          showNotice(e.message || 'Factory reset failed.', 'error');
        }
      })();
    });
  }

  /* SD format — lightweight (no global UI lock) */
  document.getElementById('format-sd')?.addEventListener('click', (ev) => {
    ev.preventDefault();
    document.getElementById('format-sd-dialog')?.showModal();
  });
  const fmtForm = document.getElementById('format-sd-form');
  if (fmtForm) {
    const input = document.getElementById('format-confirm-input');
    const confirmBtn = document.getElementById('format-sd-confirm');
    const closeFmt = () => {
      document.getElementById('format-sd-dialog')?.close();
      if (input) input.value = '';
      if (confirmBtn) confirmBtn.disabled = true;
    };
    ['format-sd-cancel', 'format-sd-cancel-2'].forEach((id) => {
      document.getElementById(id)?.addEventListener('click', closeFmt);
    });
    input?.addEventListener('input', () => {
      if (confirmBtn) confirmBtn.disabled = input.value.trim().toUpperCase() !== 'FORMAT';
    });
    fmtForm.addEventListener('submit', (ev) => {
      ev.preventDefault();
      if (!input || input.value.trim().toUpperCase() !== 'FORMAT') return;
      (async () => {
        try {
          await apiRequest('/sd/format', { method: 'POST' });
          showNotice('SD format started.', 'success');
          closeFmt();
        } catch (e) {
          showNotice('SD format failed: ' + (e.message || ''), 'error');
        }
      })();
    });
  }

  Neo2.openSettings = openSettings;
})();

/**
 * Neo Link page — uses js/core.js for auth/header/status/logs.
 */
(function () {
  if (document.body?.dataset?.page !== 'neo-link') return;

  const $ = (id) => document.getElementById(id);
  const authFetch = (path, opts) => window.Neo2.authFetch(path, opts);
  const readJson = (res) => window.Neo2.readJson(res);
  const token = () => window.Neo2.getAuthToken();

  function setStatus(msg, ok) {
    const el = $('link-form-status');
    if (!el) return;
    el.textContent = msg || '';
    el.style.color = ok === false ? 'var(--coral, #a33)' : '';
  }

  function fillForm(cfg) {
    $('link-enabled').checked = !!cfg.enabled;
    $('link-base-url').value = cfg.base_url || '';
    $('link-model').value = cfg.model || 'gpt-5-nano';
    $('link-system').value = cfg.system || '';
    $('link-max-tokens').value = cfg.max_tokens || 450;
    $('link-max-rpm').value = cfg.max_rpm || 6;
    $('link-context-turns').value = cfg.context_turns ?? 2;
    $('link-api-key').value = '';
    const hint = $('link-key-hint');
    if (cfg.has_api_key) {
      hint.hidden = false;
      hint.textContent = 'Saved key on device' + (cfg.api_key_hint ? ` (${cfg.api_key_hint})` : '');
    } else {
      hint.hidden = true;
    }
  }

  async function loadConfig() {
    const res = await authFetch('/link/llm');
    const cfg = await readJson(res);
    if (!res.ok) throw new Error(cfg.error || 'Failed to load LLM config');
    fillForm(cfg);
  }

  async function refreshLinkStatus() {
    try {
      const [stRes, lastRes] = await Promise.all([
        authFetch('/link/status'),
        authFetch('/link/last'),
      ]);
      const st = await readJson(stRes);
      const last = await readJson(lastRes).catch(() => ({}));
      if (!stRes.ok) throw new Error(st.error || 'Status failed');
      $('link-llm-state').textContent = st.llm_ready ? 'Ready' : st.llm_enabled ? 'Not ready' : 'Off';
      $('link-llm-detail').textContent = st.llm_last_error
        ? st.llm_last_error
        : st.llm_model
          ? `${st.llm_model} · ${st.llm_max_rpm || '?'} req/min`
          : 'Configure endpoint below';
      $('link-mailbox-state').textContent = st.mailbox || '—';
      $('link-mailbox-detail').textContent = st.alphaword_coupled
        ? 'WARNING: AlphaWord coupling'
        : 'NeoLinkIn only (applet 0xA1C0)';
      $('link-context-state').textContent = `${st.llm_context_stored || 0}/${st.llm_context_turns ?? 2}`;
      $('link-context-detail').textContent = 'Follow-ups use prior turns on the buddy';
      const installDetail = $('link-install-detail');
      if (installDetail) {
        const bundled = st.applet_bundled_version || '?';
        const installed = st.applet_installed_version
          ? `${st.applet_installed_version} ram=${st.applet_installed_ram || '?'}`
          : 'not installed';
        if (st.applet_sync_busy) {
          installDetail.textContent = 'Checking / updating Neo Link Chat on Neo…';
        } else if (st.applet_up_to_date) {
          installDetail.textContent = `OK · bundled ${bundled} · Neo has ${installed}`;
        } else if (st.applet_needs_update) {
          installDetail.textContent = `UPDATE NEEDED · bundled ${bundled} · Neo has ${installed}`;
        } else {
          installDetail.textContent = `bundled ${bundled} · ${st.neo_connected ? installed : 'Neo not connected'}`;
        }
        if (st.applet_sync_msg) {
          installDetail.textContent += ` · ${st.applet_sync_msg}`;
        }
      }
      const box = $('link-last-box');
      if (box) {
        box.textContent = `Prompt:\n${last.prompt || '(none)'}\n\nReply:\n${last.reply || '(none)'}`;
      }
    } catch (e) {
      $('link-llm-state').textContent = '—';
      $('link-llm-detail').textContent = e.message || 'Sign in';
    }
  }

  Neo2.onLogin = () => {
    loadConfig().then(refreshLinkStatus).catch((e) => setStatus(e.message, false));
  };

  $('link-llm-form')?.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    setStatus('Saving…');
    const body = {
      enabled: $('link-enabled').checked,
      base_url: $('link-base-url').value.trim(),
      model: $('link-model').value.trim(),
      system: $('link-system').value,
      max_tokens: Number($('link-max-tokens').value) || 450,
      max_rpm: Number($('link-max-rpm').value) || 6,
      context_turns: Number($('link-context-turns').value) || 0,
    };
    const key = $('link-api-key').value;
    if (key.length) body.api_key = key;
    try {
      const res = await authFetch('/link/llm', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      const j = await readJson(res);
      if (!res.ok) throw new Error(j.error || 'Save failed');
      setStatus('Saved.', true);
      $('link-api-key').value = '';
      await loadConfig();
      await refreshLinkStatus();
    } catch (e) {
      setStatus(e.message || 'Save failed', false);
    }
  });

  $('link-clear-key')?.addEventListener('click', async () => {
    try {
      const res = await authFetch('/link/llm', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ api_key: '' }),
      });
      const j = await readJson(res);
      if (!res.ok) throw new Error(j.error || 'Clear failed');
      setStatus('API key cleared.', true);
      await loadConfig();
    } catch (e) {
      setStatus(e.message || 'Clear failed', false);
    }
  });

  $('link-clear-context')?.addEventListener('click', async () => {
    try {
      const res = await authFetch('/link/llm/clear-context', { method: 'POST' });
      const j = await readJson(res);
      if (!res.ok) throw new Error(j.error || 'Clear failed');
      setStatus('Conversation context cleared.', true);
      await refreshLinkStatus();
    } catch (e) {
      setStatus(e.message || 'Clear failed', false);
    }
  });

  $('link-test')?.addEventListener('click', async () => {
    setStatus('Calling API…');
    try {
      const res = await authFetch('/link/llm/test', {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: 'Reply with exactly: Neo Link OK',
      });
      const j = await readJson(res);
      if (!j.ok) {
        const msg = j.error || j.detail || 'Test failed';
        const detail = $('link-llm-detail');
        if (detail) detail.textContent = msg;
        $('link-llm-state').textContent = 'Error';
        throw new Error(msg);
      }
      setStatus('Test OK: ' + (j.reply || '').slice(0, 120), true);
      await refreshLinkStatus();
    } catch (e) {
      setStatus(e.message || 'Test failed', false);
    }
  });

  $('link-install-applet')?.addEventListener('click', async () => {
    if (
      !window.confirm(
        'Install Neo Link Chat on the Neo?\n\nThe Neo leaves keyboard mode while the applet is written. An existing Neo Link Chat install is replaced.'
      )
    ) {
      return;
    }
    setStatus('Installing Neo Link Chat…');
    try {
      const res = await authFetch('/link/install-applet', {
        method: 'POST',
        headers: { 'X-Neo-Replace': 'true' },
      });
      const j = await readJson(res);
      if (!res.ok || !j.ok) throw new Error(j.error || 'Install failed');
      setStatus(
        `Installed ${j.name || 'Neo Link Chat'} (0x${(j.applet_id || 0xa1c0).toString(16).toUpperCase()}, ${j.bytes || '?'} bytes). Power-cycle with Left Shift+Tab to open it.`,
        true
      );
      await refreshLinkStatus();
    } catch (e) {
      setStatus(e.message || 'Install failed', false);
    }
  });

  $('link-path-send')?.addEventListener('click', async () => {
    setStatus('Writing NeoLinkIn…');
    try {
      const res = await authFetch('/link/send', {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: 'Path1 OK: press Find on Neo Link Chat.',
      });
      const j = await readJson(res);
      if (!j.ok) throw new Error(j.error || 'Send failed');
      setStatus('Path 1 done — on Neo: open Neo Link Chat → Find.', true);
      await refreshLinkStatus();
    } catch (e) {
      setStatus(e.message || 'Send failed', false);
    }
  });

  $('link-path-pull')?.addEventListener('click', async () => {
    setStatus('Reading NeoLinkOut + calling LLM…');
    try {
      const res = await authFetch('/link/pull', { method: 'POST' });
      const j = await readJson(res);
      if (!j.ok) throw new Error(j.error || 'Pull failed — Send a prompt from the applet first');
      setStatus('Path 2 queued — wait, then Find on Neo. Prompt: ' + (j.prompt || '').slice(0, 60), true);
      await refreshLinkStatus();
    } catch (e) {
      setStatus(e.message || 'Pull failed', false);
    }
  });

  $('link-refresh-status')?.addEventListener('click', () => {
    refreshLinkStatus().catch(() => {});
  });

  async function boot() {
    if (!token()) {
      setStatus('Sign in to configure Neo Link.', false);
      return;
    }
    try {
      await loadConfig();
      await refreshLinkStatus();
      setStatus('');
    } catch (e) {
      setStatus(e.message || 'Load failed', false);
    }
  }

  boot();
  setInterval(() => {
    if (token()) refreshLinkStatus().catch(() => {});
  }, 12000);
})();

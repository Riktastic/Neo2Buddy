/**
 * Static portal showcase — sample data for GitHub Pages and local ?demo=1 previews.
 * No-ops on real buddy hosts (LAN IP / .local).
 */
(function () {
  const API_BASE = '/api/v1';

  function isDemoMode() {
    const params = new URLSearchParams(location.search);
    if (params.get('demo') === '0') return false;
    if (params.get('demo') === '1') return true;
    const host = location.hostname;
    if (host === 'localhost' || host === '127.0.0.1') return params.has('demo');
    if (/\.github\.io$/i.test(host)) return true;
    if (/^192\.168\./.test(host) || host.endsWith('.local') || host === '192.168.4.1') return false;
    return false;
  }

  if (!isDemoMode()) return;

  window.NEO2_PORTAL_DEMO = true;

  const DEMO_APPLETS = [
    { id: 40960, name: 'AlphaWord', file_count: 8, rom_size: 24576 },
    { id: 40961, name: 'Calculator', file_count: 1, rom_size: 8192 },
    { id: 40962, name: 'French', file_count: 1, rom_size: 12288 },
    { id: 40963, name: 'Spanish', file_count: 1, rom_size: 12288 },
  ];

  const DEMO_NEO_FILES = [
    { name: 'Chapter one', applet_id: 40960, applet_name: 'AlphaWord', file_index: 1, size: 1842, alloc_size: 2033, used_size: 1842 },
    { name: 'Morning pages', applet_id: 40960, applet_name: 'AlphaWord', file_index: 2, size: 956, alloc_size: 1867, used_size: 956 },
    { name: 'Notes', applet_id: 40960, applet_name: 'AlphaWord', file_index: 3, size: 412, alloc_size: 512, used_size: 412 },
    { name: 'Ideas', applet_id: 40960, applet_name: 'AlphaWord', file_index: 4, size: 0, alloc_size: 512, used_size: 0 },
  ];

  const DEMO_BACKUPS = [
    { name: 'RiksNeo_s01_Chapter_one_20260808.txt', size: 1842, modified: Math.floor(Date.now() / 1000) - 3600 },
    { name: 'RiksNeo_s02_Morning_pages_20260807.txt', size: 956, modified: Math.floor(Date.now() / 1000) - 86400 },
  ];

  const DEMO_SAMPLE_DOC =
    'It was a bright cold day in April, and the clocks were striking thirteen.\n\n' +
    'Winston Smith, his chin nuzzled into his breast in an effort to escape the vile wind, ' +
    'slipped quickly through the glass doors of Victory Mansions, though not quickly enough ' +
    'to prevent a swirl of gritty dust from entering along with him.';

  function demoNeoFileText(fileIndex) {
    const file = DEMO_NEO_FILES.find((f) => f.file_index === fileIndex);
    if (!file || !(file.used_size || file.size)) {
      return '';
    }
    if (fileIndex === 1) {
      return DEMO_SAMPLE_DOC;
    }
    return (
      `${file.name}\n\n` +
      'Sample Neo document for the portal showcase.\n\n' +
      'On a real buddy, this would be UTF-8 text read from the AlphaWord file on your Neo2.'
    );
  }

  function demoNeoBackupPath(fileIndex) {
    const file = DEMO_NEO_FILES.find((f) => f.file_index === fileIndex);
    const safeName = (file?.name || `file_${fileIndex}`).replace(/\s+/g, '_');
    return `/sdcard/neo/RiksNeo_s${String(fileIndex).padStart(2, '0')}_${safeName}_20260808.txt`;
  }

  function textResponse(body) {
    return new Response(body, { status: 200, headers: { 'Content-Type': 'text/plain; charset=utf-8' } });
  }

  let demoLiveText = '';
  let demoLiveSeq = 0;
  let demoLiveTick = 0;
  let demoAutoBackupOnConnect = true;

  function jsonResponse(body, status = 200) {
    return new Response(JSON.stringify(body), {
      status,
      headers: { 'Content-Type': 'application/json' },
    });
  }

  function emptyOk() {
    return jsonResponse({ ok: true });
  }

  function routeDemoApi(path, method, init) {
    const raw = path.replace(API_BASE, '');
    const qmark = raw.indexOf('?');
    const p = qmark >= 0 ? raw.slice(0, qmark) : raw;
    const query = qmark >= 0 ? raw.slice(qmark + 1) : '';
    const m = (method || 'GET').toUpperCase();

    if (p === '/login' && m === 'POST') {
      return jsonResponse({ token: 'demo-token', expires_in: 3600 });
    }

    if (p === '/status' || p === '/usb/status') {
      return jsonResponse({
        usb_connected: true,
        usb_keyboard_active: true,
        usb_neo_ready: false,
        product: 'AlphaSmart Neo2',
        usb_bus_devices: 1,
        usb_host_active: true,
        usb_flipping: false,
        ip: '192.168.8.244',
        ble_state: 'idle',
        have_sdcard: true,
        have_oled: true,
        auto_backup_busy: false,
        auto_backup_on_connect: demoAutoBackupOnConnect,
      });
    }

    if (p === '/command/info') {
      return jsonResponse({ version: 'Neo2', free_rom: 120832, free_ram: 32768 });
    }
    if (p === '/command/mode') {
      return jsonResponse({ mode: 'keyboard' });
    }
    if (p === '/command/list_applets') {
      return jsonResponse(DEMO_APPLETS);
    }

    if (p === '/neo/files') {
      return jsonResponse(DEMO_NEO_FILES.filter((f) => (f.used_size || f.size || 0) > 0));
    }

    const neoFileTransfer = p.match(/^\/neo\/applets\/(\d+)\/files\/(\d+)\/(download|read)$/);
    if (neoFileTransfer) {
      const fileIndex = parseInt(neoFileTransfer[2], 10);
      const action = neoFileTransfer[3];
      const text = demoNeoFileText(fileIndex);
      if (action === 'download' && m === 'GET') {
        return textResponse(text);
      }
      if (action === 'read' && m === 'POST') {
        if (query.includes('backup=1')) {
          return jsonResponse({ saved: true, path: demoNeoBackupPath(fileIndex) });
        }
        return textResponse(text);
      }
    }

    if (/^\/neo\/applets\/\d+\/files\/read-all$/.test(p) && m === 'POST') {
      const count = DEMO_NEO_FILES.filter((f) => (f.used_size || f.size || 0) > 0).length;
      return jsonResponse({ count, returned_to_keyboard: true });
    }

    const appletDownload = p.match(/^\/neo\/applets\/(\d+)\/download$/);
    if (appletDownload && m === 'GET') {
      const applet = DEMO_APPLETS.find((a) => a.id === parseInt(appletDownload[1], 10));
      const label = applet ? applet.name : 'applet';
      return new Response(`Demo SmartApplet package (${label}) — not a real .os3kapp file.`, {
        status: 200,
        headers: { 'Content-Type': 'application/octet-stream' },
      });
    }

    if (p === '/files' && m === 'GET') {
      return jsonResponse(DEMO_BACKUPS);
    }

    if (p.startsWith('/files/view')) {
      return jsonResponse({ name: 'Chapter one', content: DEMO_SAMPLE_DOC });
    }
    if (p.startsWith('/files/download')) {
      return textResponse(DEMO_SAMPLE_DOC);
    }

    if (p === '/keyboard/recent') {
      demoLiveTick += 1;
      if (demoLiveTick % 2 === 0 && demoLiveText.length < DEMO_SAMPLE_DOC.length) {
        demoLiveText = DEMO_SAMPLE_DOC.slice(0, demoLiveText.length + 3);
        demoLiveSeq += 1;
      }
      return jsonResponse({ text: demoLiveText, sequence: demoLiveSeq });
    }

    if (p.startsWith('/keyboard/raw')) {
      const now = Math.floor(Date.now() / 1000);
      return jsonResponse([
        { ts: now, hex: '00 00 17 00 00 00 00 00', keys: 't' },
        { ts: now - 1, hex: '00 00 0b 00 00 00 00 00', keys: 'h' },
        { ts: now - 2, hex: '00 00 0e 00 00 00 00 00', keys: 'e' },
      ]);
    }

    if (p === '/keyboard/clear' && m === 'POST') {
      demoLiveText = '';
      demoLiveSeq += 1;
      return emptyOk();
    }

    if (p === '/settings' && m === 'POST') {
      try {
        const body = init?.body ? JSON.parse(init.body) : {};
        if (typeof body.auto_backup_on_connect === 'boolean') {
          demoAutoBackupOnConnect = body.auto_backup_on_connect;
        }
      } catch (e) {
        // ignore malformed demo payloads
      }
      return emptyOk();
    }

    if (p === '/settings') {
      return jsonResponse({
        device_name: 'Neo2 Buddy',
        neo_label: 'RiksNeo',
        keyboard_layout: 'us',
        sleep_timeout_seconds: 600,
        require_portal_auth: true,
        auto_backup_on_connect: demoAutoBackupOnConnect,
        auto_cloud_sync_after_backup: false,
        network_mode: 'home',
        hotspot_ssid: 'Neo2-Buddy',
        wifi_ssid: 'HomeNetwork',
        wifi_dhcp: true,
      });
    }

    if (p === '/wifi') {
      return jsonResponse({
        network_mode: 'home',
        ssid: 'HomeNetwork',
        ip: '192.168.8.244',
      });
    }

    if (p === '/wifi/scan') {
      return jsonResponse([
        { ssid: 'HomeNetwork', rssi: -58 },
        { ssid: 'Guest WiFi', rssi: -72 },
        { ssid: 'IoT', rssi: -81 },
      ]);
    }

    if (p === '/sync/config') {
      return jsonResponse({
        provider: 'webdav',
        enabled: true,
        endpoint: 'https://cloud.example.com/remote.php/dav/files/writer/',
        folder: 'neo-backups',
        username: 'writer',
        credentials_configured: true,
        health: {
          state: 'ok',
          ready: true,
          configured: true,
          wifi_ok: true,
          clock_ok: true,
          issues: [],
        },
        status: {
          last_test_ok: true,
          last_test_message: 'Demo: connection OK',
          last_ok: true,
          last_message: 'Demo showcase — no uploads run here.',
        },
      });
    }

    if (p === '/ble') {
      return jsonResponse({ state: 'idle', can_send: false, bonded_hosts: 0 });
    }

    if (p === '/neo/autobackup') {
      return jsonResponse({ busy: false, last_result: 'ESP_OK' });
    }

    if (p === '/logs') {
      return jsonResponse([
        { ts_ms: Date.now(), level: 'INFO', message: 'Demo mode — sample portal data only.' },
        { ts_ms: Date.now() - 5000, level: 'INFO', message: 'Neo keyboard listener started (PID 0xBD04)' },
        { ts_ms: Date.now() - 12000, level: 'INFO', message: 'Home network connected. IP=192.168.8.244' },
      ]);
    }

    if (p === '/sd/status') {
      return jsonResponse({ size_bytes: 16 * 1024 * 1024 * 1024, formatting: false });
    }

    if (m === 'POST' || m === 'DELETE' || m === 'PUT') {
      return emptyOk();
    }

    return jsonResponse({ error: 'Demo: endpoint not mocked', path: p }, 404);
  }

  const nativeFetch = window.fetch.bind(window);
  window.fetch = function demoFetch(input, init) {
    const url = typeof input === 'string' ? input : input.url;
    if (url.includes(API_BASE)) {
      const path = url.slice(url.indexOf(API_BASE));
      return Promise.resolve(routeDemoApi(path, init?.method, init));
    }
    return nativeFetch(input, init);
  };

  function installDemoStorage() {
    localStorage.setItem('neo2_token', 'demo-token');
    localStorage.setItem('neo2_setup_complete', 'true');
    localStorage.setItem('neo2_device_name', 'Neo2 Buddy');
    sessionStorage.setItem('neo_applet_count', String(DEMO_APPLETS.length));
  }

  function installDemoBanner() {
    if (document.getElementById('portal-demo-banner')) return;
    document.documentElement.classList.add('portal-demo');
    const banner = document.createElement('div');
    banner.id = 'portal-demo-banner';
    banner.className = 'portal-demo-banner';
    banner.setAttribute('role', 'status');
    banner.innerHTML =
      '<strong>Portal showcase (beta)</strong> — sample data only; firmware is work in progress. ' +
      'Tested so far on a <strong>Dutch Neo2</strong> only — UK/US keyboard layouts may differ. ' +
      'This is a static preview; USB, backups, and cloud sync do not run here. ' +
      '<a href="https://github.com/Riktastic/Neo2Buddy#alphasmart-neo2-buddy">Flash a buddy</a> for the real thing.';
    document.body.prepend(banner);
  }

  installDemoStorage();
  if (document.body) installDemoBanner();
  else document.addEventListener('DOMContentLoaded', installDemoBanner);
})();

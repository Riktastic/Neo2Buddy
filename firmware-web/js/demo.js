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
    { id: 0xa000, name: 'AlphaWord Plus', file_count: 8, rom_size: 24576, ram_size: 8192 },
    { id: 0xa001, name: 'Calculator', file_count: 0, rom_size: 8192, ram_size: 2048 },
    { id: 0xa002, name: 'SpellCheck', file_count: 1, rom_size: 98304, ram_size: 4096 },
    { id: 0xa003, name: 'Thesaurus', file_count: 1, rom_size: 65536, ram_size: 4096 },
    { id: 0xa004, name: 'KAZ Typing Tutor', file_count: 1, rom_size: 18432, ram_size: 2048 },
    { id: 0xa005, name: 'Control Panel', file_count: 0, rom_size: 12288, ram_size: 2048 },
    { id: 0xa1b6, name: 'Flash Cards', file_count: 1, rom_size: 5056, ram_size: 2048 },
  ];

  const DEMO_NEO_FILES = [
    { name: 'Chapter one', applet_id: 0xa000, applet_name: 'AlphaWord Plus', file_index: 1, size: 1842, alloc_size: 2048, used_size: 1842 },
    { name: 'Morning pages', applet_id: 0xa000, applet_name: 'AlphaWord Plus', file_index: 2, size: 1256, alloc_size: 2048, used_size: 1256 },
    { name: 'School notes', applet_id: 0xa000, applet_name: 'AlphaWord Plus', file_index: 3, size: 892, alloc_size: 1024, used_size: 892 },
    { name: 'Ideas', applet_id: 0xa000, applet_name: 'AlphaWord Plus', file_index: 4, size: 318, alloc_size: 512, used_size: 318 },
    { name: 'Briefing', applet_id: 0xa000, applet_name: 'AlphaWord Plus', file_index: 5, size: 674, alloc_size: 1024, used_size: 674 },
    { name: 'Boodschappen', applet_id: 0xa000, applet_name: 'AlphaWord Plus', file_index: 6, size: 241, alloc_size: 512, used_size: 241 },
    { name: 'File 7', applet_id: 0xa000, applet_name: 'AlphaWord Plus', file_index: 7, size: 0, alloc_size: 512, used_size: 0 },
    { name: 'File 8', applet_id: 0xa000, applet_name: 'AlphaWord Plus', file_index: 8, size: 0, alloc_size: 512, used_size: 0 },
  ];

  const nowSec = Math.floor(Date.now() / 1000);
  const DEMO_BACKUPS = [
    { name: 'RiksNeo_s01_Chapter_one_20260812.txt', size: 1842, modified: nowSec - 3600 },
    { name: 'RiksNeo_s02_Morning_pages_20260812.txt', size: 1256, modified: nowSec - 7200 },
    { name: 'RiksNeo_s03_School_notes_20260811.txt', size: 892, modified: nowSec - 86400 },
    { name: 'RiksNeo_s04_Ideas_20260811.txt', size: 318, modified: nowSec - 90000 },
    { name: 'RiksNeo_s05_Briefing_20260810.txt', size: 674, modified: nowSec - 172800 },
    { name: 'RiksNeo_s06_Boodschappen_20260810.txt', size: 241, modified: nowSec - 176400 },
    { name: 'RiksNeo_s01_Chapter_one_20260808.txt', size: 1710, modified: nowSec - 4 * 86400 },
  ];

  const DEMO_SAMPLE_DOC =
    'It was a bright cold day in April, and the clocks were striking thirteen.\n\n' +
    'Winston Smith, his chin nuzzled into his breast in an effort to escape the vile wind, ' +
    'slipped quickly through the glass doors of Victory Mansions, though not quickly enough ' +
    'to prevent a swirl of gritty dust from entering along with him.';

  const DEMO_FILE_TEXT = {
    1: DEMO_SAMPLE_DOC,
    2:
      'Morning pages — 12 August\n\n' +
      'Write first, edit later. The Neo is good at that.\n\n' +
      'Three things for today: finish chapter one, pack the cable, send a copy to the laptop.',
    3:
      'Geschiedenis — les 4\n\n' +
      '- Industriële revolutie: stoom, spoor, steden\n' +
      '- Huiswerk: blz. 48–51, vragen 1 t/m 6\n' +
      '- Toets vrijdag: begrippenlijst leren',
    4:
      'Ideas\n\n' +
      '- Quiet writing desk, no tabs\n' +
      '- Backup when I plug in, not when I remember\n' +
      '- Bluetooth into Docs on the kitchen laptop',
    5:
      'Friday briefing\n\n' +
      'Keep the Neo for drafting. Buddy for backups and a phone preview.\n' +
      'If the cable is packed, the machine stays a typewriter.',
    6:
      'Boodschappen\n\n' +
      'brood, melk, koffie, kaas, appels, batterijen AA (3x), usb-kabel',
  };

  const demoFlashDecks = {
    'en-nl-basic': {
      name: 'English to Dutch',
      cards: [
        { front: 'hello', back: 'hallo' },
        { front: 'goodbye', back: 'tot ziens' },
        { front: 'please', back: 'alsjeblieft' },
        { front: 'thank you', back: 'dank je' },
        { front: 'yes', back: 'ja' },
        { front: 'no', back: 'nee' },
        { front: 'good morning', back: 'goedemorgen' },
        { front: 'how are you?', back: 'hoe gaat het?' },
        { front: 'I', back: 'ik' },
        { front: 'you', back: 'jij' },
        { front: 'water', back: 'water' },
        { front: 'bread', back: 'brood' },
        { front: 'milk', back: 'melk' },
        { front: 'house', back: 'huis' },
        { front: 'friend', back: 'vriend' },
        { front: 'today', back: 'vandaag' },
      ],
    },
    'nl-irregular': {
      name: 'Dutch irregular verbs',
      cards: [
        { front: 'to be', back: 'zijn / was / geweest' },
        { front: 'to have', back: 'hebben / had / gehad' },
        { front: 'to go', back: 'gaan / ging / gegaan' },
        { front: 'to come', back: 'komen / kwam / gekomen' },
        { front: 'to see', back: 'zien / zag / gezien' },
        { front: 'to do', back: 'doen / deed / gedaan' },
        { front: 'to think', back: 'denken / dacht / gedacht' },
        { front: 'to bring', back: 'brengen / bracht / gebracht' },
      ],
    },
  };

  function demoNeoFileText(fileIndex) {
    const file = DEMO_NEO_FILES.find((f) => f.file_index === fileIndex);
    if (!file || !(file.used_size || file.size)) {
      return '';
    }
    return DEMO_FILE_TEXT[fileIndex] || `${file.name}\n\nSample Neo document for the portal showcase.`;
  }

  function demoBackupText(name) {
    const match = String(name || '').match(/_s0?(\d+)_/);
    const idx = match ? parseInt(match[1], 10) : 1;
    return demoNeoFileText(idx) || DEMO_SAMPLE_DOC;
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
  let demoManagerMode = false;

  function demoUsbStatus() {
    return {
      usb_connected: true,
      usb_keyboard_active: !demoManagerMode,
      usb_neo_ready: demoManagerMode,
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
    };
  }

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
    if (p === '/token/refresh' && m === 'POST') {
      return jsonResponse({ token: 'demo-token', expires_in: 3600 });
    }
    if (p === '/onboarding' && m === 'GET') {
      return jsonResponse({ onboarding_complete: true });
    }

    if ((p === '/neo/manager' || p === '/neo/rescan') && m === 'POST') {
      demoManagerMode = true;
      if (p === '/neo/rescan') {
        return jsonResponse({ ok: true, neo_ready: true, bus_devices: 1 });
      }
      return emptyOk();
    }
    if (p === '/neo/restart' && m === 'POST') {
      demoManagerMode = false;
      return emptyOk();
    }

    if (p === '/status' || p === '/usb/status') {
      return jsonResponse(demoUsbStatus());
    }

    if (p === '/command/info') {
      return jsonResponse({
        version: 'Neo2',
        free_rom: 120832,
        free_ram: 32768,
        mode: demoManagerMode ? 'manager' : 'keyboard',
      });
    }
    if (p === '/command/mode') {
      return jsonResponse({ mode: demoManagerMode ? 'manager' : 'keyboard' });
    }
    if (p === '/command/list_applets') {
      return jsonResponse(DEMO_APPLETS);
    }

    if (p === '/neo/stock-applets' && m === 'GET') {
      return jsonResponse({
        bundled_count: 11,
        applets: [
          {
            slug: 'dice-table', name: 'Dice Table', category: 'game', applet_id: 0xa1b2,
            blurb: 'D&D dice + notes. F1–F7 roll; Space=2d6.',
            summary: 'Tabletop companion: roll dice on the right while keeping short session notes on the left.',
            how_to: 'F1–F7 roll d4–d100. Space=2d6. F8 undo. Enter note. Find save. Clear File wipe.',
            version_major: 1, version_minor: 0, version_rev: 'a', bytes: 3550, bundled: true,
          },
          {
            slug: 'snake', name: 'Snake', category: 'game', applet_id: 0xa1b8,
            blurb: 'Eat stars, grow faster, beat your best. Space pauses.',
            summary: 'Classic snake that speeds up as you score. Pause anytime; best score is saved.',
            how_to: 'Enter start. Arrows steer (same again=nudge). Space pause. Clear File reset best.',
            version_major: 1, version_minor: 0, version_rev: 'a', bytes: 2976, bundled: true,
          },
          {
            slug: 'hang-word', name: 'Hang Word', category: 'game', applet_id: 0xa1b9,
            blurb: 'Guess the word. Tab=hint (costs a life).',
            summary: 'Hangman with streak tracking, 40 words, and optional hints.',
            how_to: 'Type letters. Tab=hint. Enter/Find=new word. Clear File reset score.',
            version_major: 1, version_minor: 0, version_rev: 'a', bytes: 3478, bundled: true,
          },
          {
            slug: 'tic-tac-toe', name: 'Tic Tac Toe', category: 'game', applet_id: 0xa1ba,
            blurb: 'Beat Neo — Easy/Hard AI. Keys 1–9 place.',
            summary: 'Beat Neo on Easy or Hard. Use arrows+Enter or the number pad.',
            how_to: 'Arrows+Enter or keys 1–9. Tab=AI level. Find=new. Clear File reset.',
            version_major: 1, version_minor: 0, version_rev: 'a', bytes: 2588, bundled: true,
          },
          {
            slug: 'task-pad', name: 'Task Pad', category: 'organize', applet_id: 0xa1b1,
            blurb: 'Checklist with save and wipe.',
            summary: 'A tiny checklist for errands, packing lists, or writing goals — saved on the Neo.',
            how_to: 'Enter add. Space toggle done. Delete remove. Find save. Clear File wipe all.',
            version_major: 1, version_minor: 0, version_rev: 'a', bytes: 2858, bundled: true,
          },
          {
            slug: 'script-pad', name: 'Script Pad', category: 'write', applet_id: 0xa1b3,
            blurb: 'Screenplay lines with speaker cues.',
            summary: 'Sketch dialogue and screenplay beats with rotating speaker cues.',
            how_to: 'Enter new line. Tab inserts next speaker (ALICE:, BOB:…). Find save. Clear File reset.',
            version_major: 1, version_minor: 0, version_rev: 'a', bytes: 2876, bundled: true,
          },
          {
            slug: 'word-tree', name: 'Word Tree', category: 'focus', applet_id: 0xa1b4,
            blurb: 'Counts AlphaWord words on the Neo; tree grows to your goal.',
            summary: 'Reads your AlphaWord files on-device and grows an ASCII tree toward a word goal.',
            how_to: 'Enter set weekly goal. Find refresh counts from AlphaWord. Clear File reset goal.',
            version_major: 1, version_minor: 0, version_rev: 'a', bytes: 3290, bundled: true,
          },
          {
            slug: 'type-drill', name: 'Type Drill', category: 'focus', applet_id: 0xa1b5,
            blurb: 'Timed WPM drill — Find peeks at next prompt.',
            summary: 'Short prompt races that measure typing speed and accuracy. Best WPM is saved.',
            how_to: 'Enter start. Find next prompt. Type exactly. Esc cancel. Clear File reset best.',
            version_major: 1, version_minor: 0, version_rev: 'a', bytes: 3674, bundled: true,
          },
          {
            slug: 'touch-type', name: 'Touch Type', category: 'focus', applet_id: 0xa1bb,
            blurb: 'Learn touch typing — lessons, finger hints, live feedback.',
            summary: 'A coach for home-row habits. Progressive exercises with realtime feedback and finger cues.',
            how_to: 'Tab pick lesson. Enter start. Type the line (misses stay put). Esc menu. Find skip.',
            version_major: 1, version_minor: 0, version_rev: 'a', bytes: 5334, bundled: true,
          },
          {
            slug: 'flash-cards', name: 'Flash Cards', category: 'learn', applet_id: 0xa1b6,
            blurb: 'English→Dutch starter + multi-set editor. Reverse & type modes.',
            summary: 'Edit named decks in the portal, push one to the Neo. On-device: Show, Reverse, Type, Type↔ with hints.',
            how_to: 'Tab cycle modes. Show/Reverse: Space+Y/N. Type: Enter check, Find=hint. Clear File=Dutch starter.',
            version_major: 1, version_minor: 0, version_rev: 'a', bytes: 5056, bundled: true,
          },
          {
            slug: 'math-drill', name: 'Math Drill', category: 'learn', applet_id: 0xa1b7,
            blurb: 'Arith, algebra, units — streak celebrations.',
            summary: 'Text-only math and science practice with immediate feedback and best streak.',
            how_to: 'Enter answer. Tab cycle Mix/Arith/Algebra/Units. Find skip. Clear File reset.',
            version_major: 1, version_minor: 0, version_rev: 'a', bytes: 4316, bundled: true,
          },
        ],
      });
    }
    if (/^\/neo\/stock-applets\/[^/]+\/install$/.test(p) && m === 'POST') {
      return jsonResponse({ ok: true });
    }
    if (p === '/neo/stock-applets/flash-cards/deck' && m === 'POST') {
      return jsonResponse({ ok: true });
    }

    if (p === '/flashdecks' && m === 'GET') {
      const decks = Object.keys(demoFlashDecks).map((id) => ({
        id,
        name: demoFlashDecks[id].name,
        cards: demoFlashDecks[id].cards.length,
      }));
      return jsonResponse({ decks });
    }
    {
      const pushMatch = p.match(/^\/flashdecks\/([^/]+)\/push$/);
      if (pushMatch && m === 'POST') {
        const id = decodeURIComponent(pushMatch[1]);
        if (!demoFlashDecks[id]) return jsonResponse({ error: 'not_found' }, 404);
        return jsonResponse({ ok: true });
      }
      const deckMatch = p.match(/^\/flashdecks\/([^/]+)$/);
      if (deckMatch) {
        const id = decodeURIComponent(deckMatch[1]);
        if (m === 'GET') {
          const deck = demoFlashDecks[id];
          if (!deck) return jsonResponse({ error: 'not_found' }, 404);
          return jsonResponse({ id, name: deck.name, cards: deck.cards });
        }
        if (m === 'PUT') {
          let body = {};
          try {
            body = JSON.parse(init?.body || '{}');
          } catch (_) {
            return jsonResponse({ error: 'bad_json' }, 400);
          }
          const cards = Array.isArray(body.cards)
            ? body.cards
                .map((c) => ({
                  front: String(c.front || '').slice(0, 23),
                  back: String(c.back || '').slice(0, 23),
                }))
                .filter((c) => c.front && c.back)
                .slice(0, 16)
            : [];
          if (!cards.length) return jsonResponse({ error: 'invalid_deck' }, 400);
          demoFlashDecks[id] = {
            name: String(body.name || id).slice(0, 40),
            cards,
          };
          return jsonResponse({ ok: true });
        }
        if (m === 'DELETE') {
          if (id === 'en-nl-basic') return jsonResponse({ error: 'protected' }, 400);
          if (!demoFlashDecks[id]) return jsonResponse({ error: 'not_found' }, 404);
          delete demoFlashDecks[id];
          return jsonResponse({ ok: true });
        }
      }
    }

    if (p === '/neo/files') {
      return jsonResponse(DEMO_NEO_FILES);
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
      const name = new URLSearchParams(query).get('name') || DEMO_BACKUPS[0].name;
      return jsonResponse({ name, content: demoBackupText(name) });
    }
    if (p.startsWith('/files/download')) {
      const name = new URLSearchParams(query).get('name') || DEMO_BACKUPS[0].name;
      return textResponse(demoBackupText(name));
    }

    if (p === '/keyboard/recent') {
      demoLiveTick += 1;
      if (demoLiveTick % 2 === 0 && demoLiveText.length < DEMO_SAMPLE_DOC.length) {
        demoLiveText = DEMO_SAMPLE_DOC.slice(0, demoLiveText.length + 3);
        demoLiveSeq += 1;
      }
      const params = new URLSearchParams(query);
      const since = params.get('since');
      const etag = `"${demoLiveSeq}:1:1"`;
      if (since !== null && Number(since) === demoLiveSeq) {
        return new Response(null, {
          status: 304,
          headers: { ETag: etag, 'Cache-Control': 'no-cache' },
        });
      }
      return new Response(
        JSON.stringify({
          text: demoLiveText,
          sequence: demoLiveSeq,
          usb_connected: true,
          usb_keyboard_active: !demoManagerMode,
          usb_neo_ready: demoManagerMode,
        }),
        {
          status: 200,
          headers: { 'Content-Type': 'application/json', ETag: etag },
        }
      );
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
      return jsonResponse({
        state: 'idle',
        can_send: false,
        bonded: 1,
        bonds: [{ addr: 'aa:bb:cc:dd:ee:ff', type: 0 }],
        bonded_hosts: 1,
        passkey: null,
        pairing_mode: 'just_works',
      });
    }
    if (p === '/ble/bonds/clear' && m === 'POST') {
      return jsonResponse({ cleared: true, bonded: 0 });
    }

    if (p === '/neo/autobackup') {
      return jsonResponse({ busy: false, last_result: 'ESP_OK' });
    }

    if (p === '/logs') {
      return jsonResponse([
        { ts_ms: Date.now(), level: 'INFO', msg: 'Demo mode — sample portal data only.' },
        { ts_ms: Date.now() - 5000, level: 'INFO', msg: 'Neo keyboard listener started (PID 0xBD04)' },
        { ts_ms: Date.now() - 12000, level: 'INFO', msg: 'Home network connected. IP=192.168.8.244' },
      ]);
    }

    if (p === '/link/status') {
      return jsonResponse({
        enabled: true,
        in_frame: false,
        last_type: 'CHAT',
        mailbox: 'ok',
        applet_bundled: true,
        applet_id: 0xa1c0,
        applet_bytes: 4516,
        neo_connected: true,
        llm_enabled: true,
        llm_ready: true,
        llm_model: 'gpt-4o-mini',
        llm_base_url: 'https://api.openai.com/v1',
        llm_has_key: true,
        llm_max_tokens: 450,
        llm_max_rpm: 6,
        llm_context_turns: 2,
        llm_context_stored: 1,
        alphaword_coupled: false,
      });
    }
    if (p === '/link/last') {
      return jsonResponse({
        prompt: 'What is a haiku?',
        reply: 'A haiku is a short poem: three lines, often 5-7-5 syllables, about a moment in nature.',
      });
    }
    if (p === '/link/llm') {
      if (m === 'GET') {
        return jsonResponse({
          enabled: true,
          base_url: 'https://api.openai.com/v1',
          model: 'gpt-4o-mini',
          system: 'You are a concise assistant for Neo Link Chat.',
          max_tokens: 450,
          max_rpm: 6,
          context_turns: 2,
          context_stored: 1,
          has_api_key: true,
          api_key_hint: '...demo',
          alphaword_coupled: false,
        });
      }
      return emptyOk();
    }
    if (p === '/link/llm/test') {
      return jsonResponse({ ok: true, reply: 'Neo Link OK' });
    }
    if (p === '/link/llm/clear-context' || p === '/link/send' || p === '/link/pull' || p === '/link/echo' || p === '/link/install-applet') {
      if (p === '/link/pull') {
        return jsonResponse({ ok: true, prompt: 'Demo prompt from NeoLinkOut', path: 'mailbox_out' });
      }
      if (p === '/link/install-applet') {
        return jsonResponse({
          ok: true,
          applet_id: 0xa1c0,
          bytes: 4516,
          replaced: true,
          name: 'Neo Link Chat',
          hint: 'Open Neo Link Chat (Left Shift+Tab at power-on).',
        });
      }
      return jsonResponse({ ok: true, delivered: true });
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
      '<strong>Portal showcase</strong> — sample data only (Neo2 Buddy <strong>1.0</strong>). ' +
      'Primary testing is on a <strong>Dutch Neo2</strong>; UK/US keyboard layouts may differ. ' +
      'This is a static preview; USB, backups, and cloud sync do not run here. ' +
      '<a href="https://github.com/Riktastic/Neo2Buddy#alphasmart-neo2-buddy">Flash a buddy</a> for the real thing.';
    document.body.prepend(banner);
  }

  installDemoStorage();
  if (document.body) installDemoBanner();
  else document.addEventListener('DOMContentLoaded', installDemoBanner);
})();

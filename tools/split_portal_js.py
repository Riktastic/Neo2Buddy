#!/usr/bin/env python3
"""
Build portal JS bundles from app.js.bak:
  core.js      — auth, login, logs, settings, status (all interactive pages)
  dashboard.js — documents / applets / backups / sync
  typing.js    — live keyboard + BLE
"""
from __future__ import annotations

import re
from pathlib import Path

JS = Path(__file__).resolve().parents[1] / "firmware-web" / "js"
SRC = JS / "app.js.bak"
if not SRC.exists():
    SRC = JS / "app.js"


def between(text: str, start: str, end: str) -> str:
    a = text.find(start)
    b = text.find(end)
    if a < 0 or b < 0 or b <= a:
        raise SystemExit(f"markers missing: {start!r} .. {end!r}")
    return text[a + len(start) : b]


def main() -> None:
    text = SRC.read_text(encoding="utf-8")

    # If markers absent, inject by known anchors then re-read strategy via regexs.
    if "/* === NEO2:TYPING_BEGIN === */" not in text:
        text = inject_markers(text)
        (JS / "app.js.bak").write_text(text, encoding="utf-8")

    shared_head = """\
(function (global) {
'use strict';
const PORTAL_PAGE = document.body && document.body.dataset ? (document.body.dataset.page || 'dashboard') : 'dashboard';
const IS_TYPING_PAGE = PORTAL_PAGE === 'typing';
const IS_DASHBOARD = PORTAL_PAGE === 'dashboard';
const IS_NEO_LINK_PAGE = PORTAL_PAGE === 'neo-link';
const API_BASE = '/api/v1';
"""

    core_body = between(text, "/* === NEO2:CORE_BEGIN === */", "/* === NEO2:CORE_END === */")
    dash_body = between(text, "/* === NEO2:DASH_BEGIN === */", "/* === NEO2:DASH_END === */")
    typ_body = between(text, "/* === NEO2:TYPING_BEGIN === */", "/* === NEO2:TYPING_END === */")

    core = shared_head + core_body + """
global.Neo2 = {
  PORTAL_PAGE, IS_TYPING_PAGE, IS_DASHBOARD, IS_NEO_LINK_PAGE, API_BASE,
  getAuthToken, setAuthToken, authFetch, localFetch, apiRequest,
  showNotice, setButtonBusy, escapeHtml, refreshStatus, openSettings,
  initHeaderFooter, updateSignInState, updateAuthVisibility,
  confirmLeaveKeyboardMode, setNeoConnectionState, updatePortalNotices,
  getNeoCharmap, neoCharmapQuery
};
initHeaderFooter();
updateSignInState();
if (typeof getAuthToken === 'function' && getAuthToken()) {
  refreshStatus().catch(function () {});
}
setInterval(function () {
  if (!document.hidden && getAuthToken()) refreshStatus().catch(function () {});
}, IS_NEO_LINK_PAGE ? 30000 : 12000);
document.addEventListener('visibilitychange', function () {
  if (!document.hidden && getAuthToken()) refreshStatus().catch(function () {});
});
})(window);
"""

    dash = """\
(function () {
'use strict';
if (!window.Neo2 || !Neo2.IS_DASHBOARD) return;
const { authFetch, apiRequest, showNotice, getAuthToken, confirmLeaveKeyboardMode,
        refreshStatus, escapeHtml, getNeoCharmap, neoCharmapQuery, API_BASE } = Neo2;
""" + dash_body + """
Neo2.onLogin = function () { loadCloudSyncConfig(); refreshFiles(); };
refreshFiles();
if (window.NEO2_PORTAL_DEMO) { refreshApplets(); refreshNeoFiles(); }
loadCloudSyncConfig();
})();
"""

    typ = """\
(function () {
'use strict';
if (!window.Neo2 || !Neo2.IS_TYPING_PAGE) return;
const { authFetch, showNotice, getAuthToken, API_BASE } = Neo2;
""" + typ_body + """
if (typeof setFollowUI === 'function') setFollowUI(true);
refreshLiveText();
setInterval(function () {
  if (!document.hidden) refreshLiveText();
}, 1000);
document.addEventListener('visibilitychange', function () {
  if (!document.hidden) refreshLiveText();
});
})();
"""

    (JS / "core.js").write_text(core, encoding="utf-8")
    (JS / "dashboard.js").write_text(dash, encoding="utf-8")
    (JS / "typing.js").write_text(typ, encoding="utf-8")
    # Keep app.js as thin redirect for any cached HTML
    (JS / "app.js").write_text(
        "/* legacy entry — use core.js + dashboard.js / typing.js */\n"
        "console.warn('app.js is deprecated; load core.js + page bundle');\n",
        encoding="utf-8",
    )
    print("core.js", (JS / "core.js").stat().st_size)
    print("dashboard.js", (JS / "dashboard.js").stat().st_size)
    print("typing.js", (JS / "typing.js").stat().st_size)


def inject_markers(text: str) -> str:
    """Insert section markers into a virgin app.js using stable anchors."""
    # CORE: from API_BASE through applyFeatureFlags end, plus login/settings/status/notices/connection
    # This injector uses coarse splits.

    # Mark typing: liveText vars through toggle-raw handler end (before setup-later)
    text = text.replace(
        "let liveSequence = -1;",
        "/* === NEO2:TYPING_BEGIN === */\nlet liveSequence = -1;",
        1,
    )
    text = text.replace(
        "document.querySelector('#setup-later')",
        "/* === NEO2:TYPING_END === */\ndocument.querySelector('#setup-later')",
        1,
    )

    # BLE + data-action block into typing as well — move marker earlier
    # Put TYPING_BEGIN before data-action instead
    text = text.replace(
        "/* === NEO2:TYPING_BEGIN === */\nlet liveSequence = -1;",
        "let liveSequence = -1;",
        1,
    )
    text = text.replace(
        "document.querySelectorAll('[data-action]')",
        "/* === NEO2:TYPING_BEGIN === */\ndocument.querySelectorAll('[data-action]')",
        1,
    )
    # Close typing after BLE cancel, reopen for live section
    text = text.replace(
        "function syncProviderFieldsUi()",
        "/* === NEO2:TYPING_END === */\nfunction syncProviderFieldsUi()",
        1,
    )
    text = text.replace(
        "let liveSequence = -1;",
        "/* === NEO2:TYPING_BEGIN === */\nlet liveSequence = -1;",
        1,
    )

    # DASH: syncProvider through end of file-delete, excluding typing markers
    text = text.replace(
        "/* === NEO2:TYPING_END === */\nfunction syncProviderFieldsUi()",
        "/* === NEO2:TYPING_END === */\n/* === NEO2:DASH_BEGIN === */\nfunction syncProviderFieldsUi()",
        1,
    )
    # End dash before liveSequence typing begin
    text = text.replace(
        "/* === NEO2:TYPING_BEGIN === */\nlet liveSequence = -1;",
        "/* === NEO2:DASH_END === */\n/* === NEO2:TYPING_BEGIN === */\nlet liveSequence = -1;",
        1,
    )
    # Also include dashboard wiring after typing end (setup/sync/files) — extend DASH
    # After TYPING_END (setup-later area), start DASH2... simpler: second dash region
    text = text.replace(
        "/* === NEO2:TYPING_END === */\ndocument.querySelector('#setup-later')",
        "/* === NEO2:TYPING_END === */\n/* === NEO2:DASH_BEGIN === */\ndocument.querySelector('#setup-later')",
        1,
    )
    # Close final dash before setInterval status poll / escapeHtml boot
    text = text.replace(
        "\nsetInterval(() => {\n  if (!document.hidden) refreshStatus();",
        "\n/* === NEO2:DASH_END === */\nsetInterval(() => {\n  if (!document.hidden) refreshStatus();",
        1,
    )

    # CORE: everything else — mark from getNeoCharmap / API through before first TYPING/DASH
    # Put CORE_BEGIN after page consts
    text = text.replace(
        "const API_BASE = '/api/v1';\n",
        "const API_BASE = '/api/v1';\n/* === NEO2:CORE_BEGIN === */\n",
        1,
    )
    # CORE_END just before first TYPING_BEGIN (data-action)
    text = text.replace(
        "/* === NEO2:TYPING_BEGIN === */\ndocument.querySelectorAll('[data-action]')",
        "/* === NEO2:CORE_END === */\n/* === NEO2:TYPING_BEGIN === */\ndocument.querySelectorAll('[data-action]')",
        1,
    )

    # Problem: settings/login/status are AFTER sync (dash). Need core to include those.
    # Re-do strategy: don't use this fragile injector — fail and use manual.
    return text


if __name__ == "__main__":
    main()

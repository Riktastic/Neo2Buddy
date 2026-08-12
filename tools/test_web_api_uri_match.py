#!/usr/bin/env python3
"""Host-side mirror of web_api_uri_match (firmware/main/web/web_api_http.c)."""


def web_api_uri_match(template: str, uri: str) -> bool:
    t = 0
    u = 0
    uri_len = len(uri)
    while t < len(template):
        if template[t] == "*":
            t += 1
            if t < len(template) and template[t] == "*":
                t += 1
                return t == len(template)
            if u >= uri_len:
                return False
            while u < uri_len and uri[u] != "/":
                u += 1
            continue
        if u >= uri_len or uri[u] != template[t]:
            return False
        t += 1
        u += 1
    return u == uri_len


def main() -> int:
    cases = [
        ("/api/v1/neo/applets/*/files/*/read", "/api/v1/neo/applets/40960/files/1/read", True),
        ("/api/v1/neo/applets/*/files/*/download", "/api/v1/neo/applets/40960/files/1/download", True),
        ("/api/v1/neo/applets/*/files/*/write", "/api/v1/neo/applets/40960/files/1/write", True),
        ("/api/v1/neo/applets/*/files/read-all", "/api/v1/neo/applets/40960/files/read-all", True),
        ("/api/v1/neo/applets/*/files/*", "/api/v1/neo/applets/40960/files/1", True),
        # Must NOT match multi-segment (old trailing-* bug → 405 on read)
        ("/api/v1/neo/applets/*/files/*", "/api/v1/neo/applets/40960/files/1/read", False),
        ("/api/v1/neo/applets/*/download", "/api/v1/neo/applets/40967/download", True),
        ("/api/v1/neo/applets/*", "/api/v1/neo/applets/40960", True),
        ("/api/v1/neo/applets/*", "/api/v1/neo/applets/40960/download", False),
        ("/api/v1/neo/applets/*", "/api/v1/neo/applets/40960/files/1/read", False),
        ("/**", "/css/portal.css", True),
        ("/**", "/js/app.js", True),
        ("/*", "/css/portal.css", False),  # single-segment only
        ("/*", "/index.html", True),
    ]
    failed = 0
    for tmpl, uri, expect in cases:
        got = web_api_uri_match(tmpl, uri)
        if got != expect:
            print(f"FAIL template={tmpl!r} uri={uri!r} got={got} expect={expect}")
            failed += 1
        else:
            print(f"OK   template={tmpl!r} uri={uri!r}")
    if failed:
        print(f"{failed} failure(s)")
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

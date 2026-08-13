# Portal demo

**[Try it out in the browser](https://riktastic.github.io/Neo2Buddy/)** — the same visual portal as on a real Buddy, with sample data. No ESP32 and no Neo required.

| Page | URL |
|------|-----|
| Documents | https://riktastic.github.io/Neo2Buddy/ |
| Typing & Bluetooth | https://riktastic.github.io/Neo2Buddy/typing.html |
| User guide | https://riktastic.github.io/Neo2Buddy/user-guide.html |

This matches the **Full** firmware profile. UART-slim images have no portal.

Nothing in the demo talks to real USB or cloud services.

## On your computer

```powershell
cd firmware-web
python -m http.server 8080
```

Open [http://localhost:8080/?demo=1](http://localhost:8080/?demo=1) and [typing.html?demo=1](http://localhost:8080/typing.html?demo=1). The `?demo=1` flag is required on localhost. On a real Buddy it stays off.

## GitHub Pages (maintainers)

The workflow `.github/workflows/pages.yml` publishes `firmware-web/` from `main`. Set the repo **Pages** source to **GitHub Actions**, then run **Deploy portal showcase** (or push a `firmware-web/` change).

How the mock API works: `firmware-web/js/demo.js`.

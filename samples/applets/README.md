# Sample SmartApplets

## HelloWorld.OS3KApp

Stock [BetaWise](https://github.com/isotherm/betawise) **v0.2** Hello World sample.

| Field | Value |
|-------|-------|
| Applet ID | `0xA1A0` |
| Name | Hello World |
| Version | 0.2 |
| Size | 518 bytes |
| Package | `C0FFEEAD` … `CAFEFEED` |

Rebuild (requires Docker Desktop):

```powershell
.\tools\build-helloworld-betawise.ps1
```

Install onto a connected Neo from the portal (**Documents → Install**) or NeoTools:

```bash
neotools applets install samples/applets/HelloWorld.OS3KApp
```

## Neo Link Chat

**Abandoned.** See [`archive/neo-link/README.md`](../../archive/neo-link/README.md).

## Stock Applet Store applets

See [`stock/README.md`](stock/README.md) — Touch Type coach, games, organizers, drills, and more.

```powershell
.\tools\build-stock-applets.ps1
```

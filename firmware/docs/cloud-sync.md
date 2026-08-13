# Cloud Sync (API)

How to use this from the portal: [docs/using.md](../../docs/using.md#cloud-copies-optional).

Device-side upload of local Neo backups to **WebDAV** (Nextcloud, ownCloud, NAS),
**S3-compatible** storage (AWS S3, Cloudflare R2, Backblaze B2, MinIO), or
**Hammer Ink** ([hammer.ink](https://hammer.ink/) — official Hammer Editor sync).

Local SD/spiflash copies remain the source of truth. A successful cloud upload
**never deletes** local files.

## Persistence

All destination settings (provider, endpoint, folder, bucket, region, username,
secret, enabled flag) are stored in NVS namespace `cloud_sync` and survive reboots.
The last connection test result and last upload summary are also persisted so the
portal can show problems after a restart.

Secrets are **never returned** in API responses or logs.

## API (authenticated)

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/api/v1/sync/config` | Provider, endpoint, folder, bucket, region, username, `credentials_configured`, `status`, `health` |
| `PUT` | `/api/v1/sync/config` | Save destination (JSON). Omit `secret` to keep the stored password/key. |
| `POST` | `/api/v1/sync/test` | Upload a tiny `.neo2buddy-test.txt` probe file |
| `POST` | `/api/v1/sync/run` | Upload all local `*.txt` backups (async worker) |

### GET `health` object

| Field | Meaning |
| --- | --- |
| `configured` | Endpoint, credentials, and bucket (if S3) are saved |
| `enabled` | Cloud upload toggle is on |
| `wifi_ok` | Buddy has home Wi‑Fi with internet |
| `clock_ok` | SNTP time is valid (required for S3 SigV4) |
| `ready` | Upload can run now |
| `state` | `idle`, `ok`, `warning`, or `error` |
| `issues` | Human-readable problem list |

### PUT body example (WebDAV / Nextcloud)

```json
{
  "provider": "webdav",
  "enabled": true,
  "endpoint": "https://cloud.example.com/remote.php/dav/files/you/backups",
  "path": "neo2",
  "username": "you",
  "secret": "app-password-here"
}
```

### PUT body example (S3 / R2)

```json
{
  "provider": "s3",
  "enabled": true,
  "endpoint": "https://ACCOUNT_ID.r2.cloudflarestorage.com",
  "bucket": "neo-backups",
  "region": "auto",
  "path": "classroom-a",
  "username": "ACCESS_KEY_ID",
  "secret": "SECRET_ACCESS_KEY"
}
```

### PUT body example (Hammer Ink)

```json
{
  "provider": "hammer",
  "enabled": true,
  "endpoint": "https://hammer.ink",
  "path": "Neo2 Buddy",
  "username": "you@example.com",
  "secret": "your-hammer-password"
}
```

Hammer uploads each local `*.txt` backup as a **Note** inside the named project
(created on first sync). Filename → note entity IDs are remembered so later
uploads overwrite the same notes. This is a one-way backup, not full Hammer
project sync.

S3 uses **path-style** URLs: `{endpoint}/{bucket}/{folder}/{filename}` with AWS
Signature Version 4. Requires a valid clock (SNTP on home Wi‑Fi).

WebDAV creates the optional subfolder automatically with `MKCOL` before the first
upload. Transient network and 5xx errors are retried up to three times.

## Auto upload after backup

When Settings → **Auto cloud upload after backup** is enabled and cloud sync is
configured, a successful Neo backup that saved one or more new files starts
`cloud_sync_start_run()` automatically. Skipped duplicates do not trigger upload.

## Portal

- **Cloud sync** (quick actions) — configure destination, checklist, test, upload now
- **Cloud upload** (Saved documents) — run sync without opening settings
- **Settings** — enable auto upload after backup

The dialog saves settings to the device before **Test connection** or **Upload now**
so tests always use the values on screen.

## Security

- HTTPS endpoints only (`https://` required)
- Use app passwords (WebDAV) or scoped access keys (S3)
- Enable flash encryption in production builds for NVS at rest

## Implementation

- `firmware/main/services/cloud_sync.c` — WebDAV PUT + MKCOL, S3 SigV4, Hammer dispatch, NVS status
- `firmware/main/services/hammer_ink.c` — Hammer protocol v3 login + project/note upload
- `firmware/main/web/web_api_http.c` — HTTP handlers
- `firmware-web/js/app.js` — setup checklist and health display

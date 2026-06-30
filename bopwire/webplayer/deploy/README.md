# Deploying the Bopwire web player

Two pieces ship independently:

1. **Front-end** → GitHub Pages (`bopwire.com`). Static files, no build.
2. **Gateway** → the VPS (`<your-vps-ip>`), reachable at `https://api.bopwire.com`
   via Caddy (auto-TLS). The browser talks only to the gateway.

---

## 1. DNS at Namecheap (do this first — Caddy needs it to get the cert)

Add an **A record** so the gateway subdomain points at the VPS:

| Type | Host | Value | TTL |
|------|------|-------|-----|
| A Record | `api` | `<your-vps-ip>` | Automatic |

That makes `api.bopwire.com → <your-vps-ip>`. Leave the apex/`www` records as they
are (they point to GitHub Pages for the site itself). Open ports **80 and 443** on
the VPS firewall — Caddy needs both to obtain and serve the Let's Encrypt cert.

Verify once it propagates:
```
nslookup api.bopwire.com 8.8.8.8     # should return <your-vps-ip>
```

## 2. Gateway on the VPS

**Build** (the target was added to the main CMake project):
```
SRC=/opt/bopwire-src/bopwire           # your checkout (adjust)
deploy/build-gateway.sh                # reconfigures + builds bopwire-web-gateway
```
The binary lands at `$SRC/build-linux/bopwire-web-gateway` and loads `libmc_rats.so`
from the build tree via RUNPATH (same as the node/mini).

**Run as a service:**
```
sudo cp deploy/bopwire-web-gateway.service /etc/systemd/system/
sudoedit /etc/systemd/system/bopwire-web-gateway.service   # fix ExecStart path
sudo systemctl daemon-reload
sudo systemctl enable --now bopwire-web-gateway
journalctl -u bopwire-web-gateway -f          # watch it join the mesh
```
You should see `bootstrap mini: …` then `listening on http://127.0.0.1:8090 (N mini(s) in mesh)`.

> Prefer the project's manual `setsid nohup` style instead of systemd? Just run
> `BOPWIRE_LISTEN_PORT=8090 ./bopwire-web-gateway` from `build-linux/`.

**Smoke-test locally on the VPS:**
```
curl -s localhost:8090/api/health
curl -s localhost:8090/api/songs | head -c 400
```

## 3. Caddy (TLS + reverse proxy)

Install Caddy (Debian/Ubuntu):
```
sudo apt install -y debian-keyring debian-archive-keyring apt-transport-https curl
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' | sudo tee /etc/apt/sources.list.d/caddy-stable.list
sudo apt update && sudo apt install -y caddy
```
Then install the site config and reload:
```
sudo cp deploy/Caddyfile /etc/caddy/Caddyfile     # or append the api.bopwire.com block
sudo mkdir -p /var/log/caddy
sudo systemctl reload caddy
```
Caddy fetches the Let's Encrypt cert automatically. Verify from your laptop:
```
curl -s https://api.bopwire.com/api/health
```

## 4. Front-end → GitHub Pages

```
deploy/publish-frontend.sh                       # clones the Pages repo + pushes
# or, against an existing clone:
deploy/publish-frontend.sh /path/to/bopwire.github.io
```
This copies `frontend/*` into `bopwire/bopwire.github.io`, keeps the `CNAME`, and
pushes. The site is live at `https://bopwire.com` after Pages rebuilds (~1 min).

> The front-end calls `https://api.bopwire.com` (see `frontend/config.js`). Until
> the gateway + DNS + Caddy are up, the site loads but shows "gateway unreachable".

## Order of operations

1. Add the `api` A record at Namecheap.
2. Build + start the gateway on the VPS.
3. Install Caddy with the `api.bopwire.com` block; confirm `https://api.bopwire.com/api/health`.
4. Publish the front-end.

## Config knobs (gateway env)

| Env | Default | Meaning |
|-----|---------|---------|
| `BOPWIRE_LISTEN_HOST` | `127.0.0.1` | Bind host (keep on loopback behind Caddy). |
| `BOPWIRE_LISTEN_PORT` | `8090` | Gateway HTTP port. |
| `BOPWIRE_VPS_HOST` / `_PORT` | `<your-vps-ip>` / `8080` | **Bootstrap** mini only; the gateway then joins the whole mesh via `mininodes.list`. |
| `BOPWIRE_ALLOWED_ORIGINS` | `https://bopwire.com,https://www.bopwire.com` | CORS allow-list. |
| `BOPWIRE_CACHE_MB` | `512` | In-memory audio cache cap. |
| `BOPWIRE_GATEWAY_ID` | (auto) | Optional fixed 40-hex librats identity. |

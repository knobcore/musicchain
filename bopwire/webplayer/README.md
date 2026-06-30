# Bopwire Web Player

A browser-based **Discover + play** experience for [bopwire.com](https://bopwire.com),
ported from the native app's Discover tab. It streams from the Bopwire P2P network
through a thin server-side **gateway** (the browser cannot speak librats directly).

```
  Browser (bopwire.com, GitHub Pages)
        │  HTTPS / JSON + range-stream
        ▼
  Caddy  ──TLS── api.bopwire.com            (VPS 85.239.238.226)
        │  reverse_proxy 127.0.0.1:8090
        ▼
  bopwire-web-gateway  (this repo, C++ / librats)
        │  librats RPC (relay.forward via mini node)
        ▼
  Mini node  ──►  Full node (songs.list / sessions)  +  Seeders (audio pieces)
```

## Why a gateway

The mini node speaks only the librats binary protocol over raw TCP (port 8080).
Browsers can only do HTTP/WebSocket/WebRTC, and `bopwire.com` is HTTPS so plaintext
or raw-TCP targets are blocked (mixed content). The gateway is a headless librats
peer that does everything the native player's data layer does — picks a full node
via `routes.get`, pulls the catalog, fetches audio from seeders, and reports plays —
and re-exposes it as a small HTTPS/JSON API the browser can use.

This is a **fresh** design, not the removed `WsMiniGateway`.

## Rewards on a web play

A play streamed through the site still mints the normal **artist + seeder + mini-node**
reward (via `session.start` / `session.heartbeat` / `session.complete`). The **listener
earns nothing** — a browser can't seed, and the gateway never sends a wallet-signed
`relay.receipt` on the listener's behalf. That gap is the whole point of the CTA:

> *Want to earn crypto listening to artists with less than 10,000 plays?* **Click here**
> → download the Linux / Android / Windows app, which *does* let the listener earn.

## Layout

| Path | What |
|------|------|
| `frontend/`      | Static site (vanilla HTML/JS/CSS, no build step). Published to the `bopwire/bopwire.github.io` Pages repo. |
| `gateway/`       | C++ gateway: librats client + HTTP server (cpp-httplib). Built on the VPS via CMake. |
| `deploy/`        | Caddyfile, systemd unit, build/deploy scripts, DNS notes. |
| `API.md`         | The HTTP contract between `frontend/` and `gateway/`. |

See [API.md](API.md) for the gateway HTTP contract and [deploy/README.md](deploy/README.md)
for VPS setup (DNS A record, Caddy, cert, service).

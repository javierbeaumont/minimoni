# Reverse proxy guides

minimoni has no built-in authentication or TLS. **Do not expose it directly to the internet.**
Front it with a reverse proxy that terminates TLS and handles auth (or with a private mesh,
keep it off the public internet entirely).

This page covers the five setups people most often ask about. Pick the first one that fits
your stack; they are listed by likely fit. Each entry includes the upstream licence so you
can choose with that in mind; only links to fully-free-software projects are included.

## Common notes

All examples assume minimoni is bound to localhost in `config.toml`:

```toml
[server]
listen = "127.0.0.1:8080"
```

The dashboard uses **Server-Sent Events** (SSE) on `/stream` for live updates. Every proxy
below needs to **stream that response without buffering**, otherwise the dashboard appears
frozen even though the daemon is fine. The gotcha-fix per proxy is called out below.

You can verify your setup with `curl -N https://your-proxy/stream`; events should land
every few seconds with no buffering.

### Limiting concurrent connections

minimoni serves every connection from a fixed pool of workers, sized from `max_dashboards` plus
a small reserve so the dashboard and the API keep answering while SSE connections are open. That
reserve is not a defence against volume: a client that opens many connections at once, or opens
them and never finishes sending the request, still ties up every worker. Capping concurrent
connections per client belongs at the proxy.

What each one can do about it differs, so check yours rather than assume:

- nginx caps concurrent connections with
  [`limit_conn`](https://nginx.org/en/docs/http/ngx_http_limit_conn_module.html), keyed on a
  variable you pick. Keying on the client address counts everyone behind one NAT address as a
  single client.
- Traefik caps simultaneous in-flight requests with
  [`inFlightReq`](https://doc.traefik.io/traefik/middlewares/http/inflightreq/). Its
  `sourceCriterion` defaults to the request host, which is one shared counter for everyone; set
  it explicitly to count per client.
- Caddy ships nothing for this. The separate
  [caddy-ratelimit](https://github.com/mholt/caddy-ratelimit) module (Apache-2.0, built in with
  xcaddy) caps request *rate*, which is a different thing from concurrency.
- Apache ships nothing for this either. What it does have is
  [`mod_reqtimeout`](https://httpd.apache.org/docs/2.4/mod/mod_reqtimeout.html), which drops
  clients that send a request too slowly: that covers the connection held open and never
  finished, but not the client that opens many at once.

On a private mesh (Tailscale, WireGuard) the question is mostly moot, since only your own
devices can open a connection at all.

## Caddy

*License: [Apache 2.0](https://caddyserver.com/) (free software).*

Recommended for homelab and single-VPS deployments. Automatic Let's Encrypt, basic auth
built in, ~5 lines of config. SSE works without extra directives.

```caddy
monitor.example.com {
    basicauth {
        admin JDJhJDE0JHh4eHh4eHh4eHh4eHh4eHh4eHh4eA==
    }
    reverse_proxy localhost:8080
}
```

Generate the bcrypt hash with `caddy hash-password`. Caddy's `reverse_proxy` directive
streams SSE without buffering by default; nothing else needed.

## nginx

*License: [BSD-2-Clause](https://nginx.org/) (free software).*

Recommended if you already run nginx for other services, or on a VPS where you want the
most-deployed proxy in production. Mature, well-documented, but more verbose than Caddy.

```nginx
server {
    listen 443 ssl http2;
    server_name monitor.example.com;

    ssl_certificate     /etc/letsencrypt/live/monitor.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/monitor.example.com/privkey.pem;

    auth_basic           "minimoni";
    auth_basic_user_file /etc/nginx/.htpasswd;

    location / {
        proxy_pass         http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header   Connection "";   # required for SSE keep-alive
        proxy_buffering    off;             # required for SSE live updates
        proxy_read_timeout 1h;              # long-lived stream
    }
}
```

Generate the htpasswd file with `htpasswd -c /etc/nginx/.htpasswd admin`. The `Connection ""`
header and `proxy_buffering off` are both required for SSE to reach the browser; without
them the dashboard appears frozen.

## Tailscale

*License: [BSD-3-Clause](https://github.com/tailscale/tailscale) for the client/daemon
(free software). **The coordination server (control plane) is proprietary SaaS** run by
Tailscale Inc.; using it means relying on that hosted service for identity, key
distribution, and ACLs. If you want a 100% free-software stack, swap the control plane for
[Headscale](https://github.com/juanfont/headscale) (BSD-3-Clause), or drop the userspace
layer altogether and run plain [WireGuard](https://www.wireguard.com/) (GPL-2.0).*

The setup below assumes the Tailscale daemon is already installed and logged in. Used here
because the OS integration is the simplest path to "dashboard reachable only from my own
devices"; choose Headscale or WireGuard for the fully-free version of the same idea.

On the host running minimoni:

```sh
sudo tailscale serve --bg --https=443 localhost:8080
```

The dashboard is now reachable at `https://<hostname>.<tailnet>.ts.net/` from any device
in your tailnet. Tailscale generates and renews the cert automatically.

If you want it accessible from the public internet (no auth; anyone with the URL gets in):

```sh
sudo tailscale funnel --bg 443 localhost:8080
```

SSE is forwarded transparently; no buffering tuning needed.

## Traefik

*License: [MIT](https://github.com/traefik/traefik) for the proxy itself (free software).
**Traefik Labs sells commercial add-ons** (Traefik Enterprise, Traefik Hub) on top of the
open core; the configuration below uses only the MIT-licensed proxy.*

Recommended if you already run Traefik (typically with Docker Compose). Configuration
lives in labels on the minimoni service.

```yaml
# docker-compose.yml excerpt
services:
  minimoni:
    image: minimoni:latest  # or run the binary directly on the host
    labels:
      - traefik.enable=true
      - traefik.http.routers.minimoni.rule=Host(`monitor.example.com`)
      - traefik.http.routers.minimoni.entrypoints=websecure
      - traefik.http.routers.minimoni.tls.certresolver=letsencrypt
      - traefik.http.services.minimoni.loadbalancer.server.port=8080
      - traefik.http.routers.minimoni.middlewares=minimoni-auth
      - traefik.http.middlewares.minimoni-auth.basicauth.users=admin:$$2y$$10$$xxxxxxxxxxxxxxxxxxx
```

The double `$$` escapes the literal `$` in YAML so Compose passes a single `$` to Traefik.
Generate the bcrypt entry with `htpasswd -nbB admin yourpassword`. Traefik streams SSE
without buffering by default; no extra middleware needed.

## Apache

*License: [Apache 2.0](https://httpd.apache.org/) (free software).*

Listed for completeness; only worth setting up if Apache is already serving other sites
on the host. Caddy or nginx are simpler from scratch.

```apache
<VirtualHost *:443>
    ServerName monitor.example.com

    SSLEngine on
    SSLCertificateFile      /etc/letsencrypt/live/monitor.example.com/fullchain.pem
    SSLCertificateKeyFile   /etc/letsencrypt/live/monitor.example.com/privkey.pem

    <Location />
        AuthType Basic
        AuthName "minimoni"
        AuthUserFile /etc/apache2/.htpasswd
        Require valid-user

        ProxyPass        http://127.0.0.1:8080/ flushpackets=on
        ProxyPassReverse http://127.0.0.1:8080/

        # SSE: disable output buffering for this location.
        SetEnv no-gzip 1
        SetEnv proxy-sendchunked 1
    </Location>
</VirtualHost>
```

Requires `mod_proxy`, `mod_proxy_http`, `mod_ssl`, and `mod_auth_basic` (all standard on
Debian/Ubuntu, enable with `a2enmod proxy proxy_http ssl auth_basic`). The
`flushpackets=on` flag on `ProxyPass` plus `proxy-sendchunked` disables Apache's reverse
proxy buffering; without them SSE updates batch up and the dashboard looks frozen.

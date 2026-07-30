# Fractal Studio FRP ingress

This bundle publishes the private development host through
`fractal.kevin0412.top` without exposing Docker, PostgreSQL, Redis, Compute, or
MinIO directly to the Internet.

## Public topology

| VPS listener | Private destination | Purpose |
| --- | --- | --- |
| `127.0.0.1:12222` | `127.0.0.1:22` | Private-host SSH via VPS jump host |
| `127.0.0.1:13010` | `127.0.0.1:3010` | Next.js frontend |
| `127.0.0.1:18110` | `127.0.0.1:18100` | Platform API |
| `127.0.0.1:19020` | `127.0.0.1:19010` | MinIO path-style downloads |
| `0.0.0.0:7000` | FRP control channel | TLS and token authenticated |

Caddy is the only public application listener. It serves ports 80/443, strips
the `/platform` prefix before sending API requests, preserves the complete
`/fractal-platform/...` path for MinIO signatures, and sends everything else
to Next.js. The release Compose overlay binds every host-published service to
`127.0.0.1`; Caddy adds HSTS and baseline browser security headers, while the
Platform API marks session cookies `Secure`.

## Prerequisites

- Ubuntu, Debian, Alibaba Cloud Linux, or another RHEL-compatible system with
  systemd.
- `x86_64` or `aarch64` CPU.
- An unproxied DNS A record for `fractal.kevin0412.top` pointing to the VPS.
- VPS cloud firewall ingress for its SSH port, TCP 80, TCP 443, and TCP 7000.
- Do **not** expose 12222, 13010, 18110, or 19020 in the cloud firewall.
- Key-only SSH access to the private host is strongly recommended.

## 1. VPS

Copy this directory to the VPS and run:

```bash
cd ops/frp
sudo ./install-frps.sh
```

The installer generates `/etc/frp/frps.token`. Retrieve it over the existing
VPS SSH session for the client installation:

```bash
sudo cat /etc/frp/frps.token
```

## 2. Private Fractal Studio host

Copy this directory to the private host and run:

```bash
cd ops/frp
sudo ./install-frpc.sh
```

The installer asks for the token without echoing it. It can alternatively read
the token from a root-readable file:

```bash
sudo FRP_TOKEN_FILE=/path/to/token ./install-frpc.sh
```

## 3. Verify on the VPS

After `frpc` connects:

```bash
sudo ./verify-server.sh
```

SSH into the private host from an existing VPS shell:

```bash
ssh -p 12222 fractal-studio@127.0.0.1
```

Or connect through the VPS from an administrator workstation:

```bash
ssh -J VPS_USER@fractal.kevin0412.top -p 12222 fractal-studio@127.0.0.1
```

## 4. Project public origins

The private host's ignored root `.env` should contain:

```dotenv
FSD_API_ORIGIN=https://fractal.kevin0412.top
FSD_S3_PUBLIC_ENDPOINT_URL=https://fractal.kevin0412.top
```

The Alipay endpoints, once the membership schema and key deployment are ready,
must be:

```dotenv
ALIPAY_NOTIFY_URL=https://fractal.kevin0412.top/platform/v1/webhooks/alipay
ALIPAY_RETURN_URL=https://fractal.kevin0412.top/payment-result
```

Do not rebuild the current Platform containers solely for this tunnel. The
recent membership change still needs database migrations and key provisioning
before a clean application rebuild is safe.

## Operations

```bash
# VPS
sudo systemctl status frps caddy --no-pager
sudo journalctl -u frps -u caddy -f

# Private host
sudo systemctl status frpc --no-pager
sudo journalctl -u frpc -f
```

To rotate the token, replace it in `/etc/frp/frps.token`, rerun
`install-frps.sh`, then rerun `install-frpc.sh` and enter the new value.

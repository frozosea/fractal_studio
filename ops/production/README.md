# VPS control plane and Node 1 Compute

These are topology templates only. Copy them to `/opt` and keep environment
files, WireGuard private keys, Alipay keys, Caddy configuration and data under
`/etc` or `/srv`; set secret files to mode `0600`. Never commit populated files.

## Invariants

- `fractal-prod` contains no Compute or payment stub. API, workers and frontend
  do not wait for Compute health, so the control plane cold-starts with zero GPU
  nodes. Gateway health remains 200; capability/preview/new-run calls return
  `503 COMPUTE_CAPACITY_EXHAUSTED` until Node 1 probes healthy.
- `fractal-node1-prod` exposes one Compute instance only on
  `10.66.0.2:18080`. Add a Node 1 firewall rich rule permitting source
  `10.66.0.1/32` to TCP 18080 and reject every other source.
- Every application image variable must name an immutable Git-SHA tag. Compose
  never builds or pulls `latest`. Load confirmed images onto the VPS before
  starting it.
- Node 1 development must use a different Compose project, ports, secrets,
  runtime and volumes. It may share the GPU, but nothing else.

## Host preparation

On Alibaba Cloud Linux 3, follow Alibaba Cloud's Docker Engine/Compose install
procedure, create a 4 GiB swapfile, install WireGuard/firewalld, enable
`net.ipv4.ip_forward=1`, open `51820/udp`, and enable Docker bridge masquerading.
Do not enable host networking for Gateway: Docker bridge traffic can route to
`10.66.0.2` through the host WireGuard interface.

Place the templates as follows:

```text
/opt/fractal-prod/docker-compose.vps.yml
/etc/fractal-prod/vps.env
/etc/fractal-prod/secrets/*
/srv/fractal-prod/{postgres,gateway-postgres,redis,minio}
/opt/fractal-node1-prod/docker-compose.node1.yml
/etc/fractal-node1-prod/compute.env
/srv/fractal-node1-prod/runtime
```

Strictly confined Snap Docker cannot read Compose files from `/opt`. Keep the
authoritative copy there, mirror the non-secret Compose file under
`/var/snap/docker/common/fractal-node1-prod`, and set `COMPUTE_RUNTIME_PATH` to
a directory below Snap's `common` directory. The systemd unit loads secrets
from `/etc` before invoking Compose; secrets are never copied into Snap storage.

Validate rendered configuration before any start:

```bash
docker compose --env-file /etc/fractal-prod/vps.env \
  -f /opt/fractal-prod/docker-compose.vps.yml config --quiet
docker compose --env-file /etc/fractal-node1-prod/compute.env \
  -f /opt/fractal-node1-prod/docker-compose.node1.yml config --quiet
```

The Node 1 service template encodes `wg0 -> Snap Docker -> production Compose`.
The VPS template encodes network/WireGuard/Docker ordering. Adjust the Docker
unit/binary only if the installed host uses a different package.

## Network verification

From the Gateway container, verify health and capabilities at
`http://10.66.0.2:18080`. Then run a real CUDA preview and durable render and
confirm ingestion lands in VPS MinIO. Verify TCP 18080 fails from Node 1 LAN,
public IP, and all WireGuard peers except `10.66.0.1`.

## Migration and rollback

Pre-sync MinIO, stop the old worker for the final 5–10 minute write freeze,
dump/restore both PostgreSQL databases, run final object sync, compare table
counts and sampled SHA-256 values, then run migrations. Preserve old volumes,
Caddy backup and the prior SHA images for at least seven days. After production
accepts writes, rollback only VPS application images; do not point traffic at an
old database that has diverged.

Before builds, prune only rebuildable BuildKit cache (for example `docker
builder prune` after reviewing it). Never run `docker system prune --volumes`.
Configure host disk alerts at 75% warning and 85% critical.

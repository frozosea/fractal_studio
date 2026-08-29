# Fractal Studio production operations

This directory contains the repository-owned templates and runbooks for one VPS control plane and
an arbitrary number of private Compute nodes.

Start here:

- [INSTALL.md](INSTALL.md) — authoritative installation, N-node expansion, release, acceptance and
  rollback procedure;
- [STATUS.md](STATUS.md) — dated read-only snapshot of the actual deployment and known drift;
- [ai-assistant.md](ai-assistant.md) — Studio AI development and production feature rollout;
- [Caddyfile.example](Caddyfile.example) — canonical production and isolated-development routes;
- [docker-compose.vps.yml](docker-compose.vps.yml) — VPS control plane only;
- [docker-compose.node1.yml](docker-compose.node1.yml) — generic one-Compute-node template with a
  legacy filename;
- [vps.env.example](vps.env.example) and [compute.env.example](compute.env.example) — unpopulated
  configuration inventories.

The older [hybrid deployment plan](../../docs/hybrid_cloud_compute_deployment_plan.md) is retained
for design history only. It describes pre-migration assumptions and is not an operating runbook.

## Production model

```text
REPLACE_PRODUCTION_ORIGIN -> VPS: Caddy + Next.js + Platform + Gateway + persistent data
                                      |
                                      | WireGuard REPLACE_WG_SUBNET
                                      v
                         0..N private Compute nodes
```

The current deployment's actual origin, WireGuard subnet and node inventory are the dated snapshot's
job ([STATUS.md](STATUS.md)); every runbook here uses `REPLACE_*` placeholders instead of those
values. See [INSTALL.md](INSTALL.md) for the fill-in process.

The dated status snapshot currently records two active nodes, but neither the Gateway database nor
scheduler has a two-node limit. `/etc/fractal-prod/vps.env` owns the complete single-line
`COMPUTE_GATEWAY_BOOTSTRAP_NODES_JSON` array. Each object defines a stable node identity, private
base URL, enabled intent and CPU/GPU capacity. Adding a node must not require editing the VPS
Compose template or application code.

## Invariants

- `fractal-prod` contains no Compute or payment stub and starts with zero GPU nodes.
- Each Compute host runs one isolated Compose project with a unique bind address, runtime and env.
- Gateway stays on Docker bridge and reaches nodes over WireGuard.
- Gateway health remains 200 with no nodes; capacity endpoints fail closed with
  `503 COMPUTE_CAPACITY_EXHAUSTED`.
- Platform owns users, commerce, quotas and assets; VPS PostgreSQL/MinIO are authoritative.
- Application images use immutable Git-SHA tags and are loaded before `up --no-build`.
- Production secrets stay under `/etc`; persistent data stays under `/srv`; populated files never
  enter Git.
- MinIO's static KMS master key is backed up and never casually removed or rotated.
- Development and production projects, ports, data, keys and lifecycle remain separate even when
  they share a GPU host.

## Repository templates versus installed files

Repository files are portable templates. Installed files may contain reviewed host-specific
differences and must never be overwritten blindly:

| Role | Repository | Installed location |
|---|---|---|
| VPS Compose | `docker-compose.vps.yml` | `/opt/fractal-prod/docker-compose.vps.yml` |
| VPS env | `vps.env.example` | `/etc/fractal-prod/vps.env` (`0600`) |
| VPS data | — | `/srv/fractal-prod/{postgres,gateway-postgres,redis,minio}` |
| Compute Compose | `docker-compose.node1.yml` | `/opt/...` or Snap `common` mirror |
| Compute env | `compute.env.example` | `/etc/<unique-project>/compute.env` (`0600`) |
| Compute runtime | — | unique host path; Snap nodes use `/var/snap/docker/common/...` |
| Caddy | `Caddyfile.example` | merge into `/etc/caddy/Caddyfile` |

Always diff the installed file against the intended template and validate rendered configuration
before an approved change.

## Safe read-only checks

VPS:

```bash
docker compose ls
docker ps --filter label=com.docker.compose.project=fractal-prod
docker compose -p fractal-prod \
  --env-file /etc/fractal-prod/vps.env \
  -f /opt/fractal-prod/docker-compose.vps.yml config --quiet
docker compose -p fractal-prod \
  --env-file /etc/fractal-prod/vps.env \
  -f /opt/fractal-prod/docker-compose.vps.yml ps -a
```

Compute host:

```bash
docker compose ls
docker ps --filter label=com.docker.compose.project=REPLACE_COMPUTE_PROJECT
docker compose -p REPLACE_COMPUTE_PROJECT \
  --env-file /etc/REPLACE_COMPUTE_PROJECT/compute.env \
  -f /opt/REPLACE_COMPUTE_PROJECT/docker-compose.node1.yml config --quiet
docker compose -p REPLACE_COMPUTE_PROJECT \
  --env-file /etc/REPLACE_COMPUTE_PROJECT/compute.env \
  -f /opt/REPLACE_COMPUTE_PROJECT/docker-compose.node1.yml ps
```

For Snap Docker, use `/snap/bin/docker` and the Compose/runtime copy below
`/var/snap/docker/common/`. Follow [INSTALL.md](INSTALL.md) before any start, stop, release,
firewall change or fault test.

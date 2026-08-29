# Fractal Studio 多节点生产安装与部署手册

本文是从空白主机安装、扩容和发布 Fractal Studio 生产环境的权威操作手册。它描述的是
**一台控制面 VPS + 0 到 N 台 Compute 节点**，不把节点数量固定为两台。当前线上实例的
当前部署的日期化快照（含真实域名、私网地址与版本号）按惯例保存在部署机本地且不入库；历史方案论证见
[hybrid_cloud_compute_deployment_plan.md](../../docs/hybrid_cloud_compute_deployment_plan.md)。

文中的 `REPLACE_*`、主机别名和节点名都必须由管理员替换。真实地址、私钥、支付配置、
供应商密钥和已填充 env 文件不得进入 Git、终端共享记录或工单正文。

## 1. 安全边界和不可变条件

- 生产 origin 与 `www` 子域由管理员提供（本模板以 `REPLACE_PRODUCTION_ORIGIN`/`REPLACE_WWW_ORIGIN` 表示）；`www` 只做保留 `{uri}` 的 308 重定向。本文所有 `REPLACE_*` 均由管理员替换为实际值；真实取值只保留在部署机本地的部署状态快照中（不入库）。
- VPS 项目固定为 `fractal-prod`，只承载 Caddy、Frontend、Platform、Gateway、两个
  PostgreSQL、Redis、MinIO 和持久数据；不运行 Compute 或支付 stub。
- 每台 Compute 主机使用独立 Compose 项目、env、runtime、WireGuard 地址和镜像；节点数量
  由 Gateway 的完整节点清单决定。
- Gateway 从 Docker bridge 经 WireGuard 访问 Compute，不使用 host network。
- Compute `18080` 只允许 VPS 的 `REPLACE_VPS_WG_IP/32` 访问，不得绑定公网可达地址。
- 控制面允许以零 Compute 节点启动。此时 health 仍为 200，而 capabilities、preview 和新
  render 必须返回 `503 COMPUTE_CAPACITY_EXHAUSTED`，且不得创建任务或扣额度。
- PostgreSQL 和 MinIO 中的数据是商业事实来源；Compute runtime 仅保存节点本地运行状态和
  临时产物。
- 应用镜像只能使用不可变 Git-SHA tag。VPS 不构建镜像，也不拉取 `latest`。
- 所有命令都显式给出 Compose 项目名和文件。禁止 `compose down -v`、全局
  `docker stop`、`docker system prune --volumes`、数据库重置和 force push。

任何停服、重启主机、故障演练或防火墙切换都需要当次明确授权。共享 Compute 主机上的其他
服务不属于本项目，尤其不得操作 Node 2 上的桌面会话、Minecraft、Docker daemon、
NetworkManager、SSHD 或 Tailscale。

## 2. 拓扑和命名

```text
Internet
  |
  v
VPS (REPLACE_VPS_WG_IP)
  Caddy -> Next.js / Platform API / MinIO
              |
              +-> Redis / Platform PostgreSQL
              +-> Compute Gateway / Gateway PostgreSQL
                            |
                            | WireGuard REPLACE_WG_SUBNET/24
                            +-> compute-<stable-key> (10.66.0.X:18080)
                            +-> compute-<stable-key> (10.66.0.Y:18080)
                            +-> ... 0 到 N 个节点
```

为每个节点先建立一行资产记录：

| 字段 | 规则 |
|---|---|
| `nodeKey` | 全局唯一且终身稳定，格式为小写字母、数字和连字符；不要复用退役节点身份 |
| WireGuard IP | `REPLACE_WG_SUBNET` 主机段内唯一的 `/32`（例如 10.66.0.2–10.66.0.254） |
| Compose 项目 | 每台主机唯一，例如 `fractal-compute-render-a-prod` |
| runtime | 该主机独立绝对路径；Snap Docker 必须位于 `/var/snap/docker/common/` 下 |
| Compute image | 与该 GPU 架构兼容的不可变 tag，例如 `...-sm89` 或 `...-sm61` |
| 槽位 | 按实测基准配置 CPU/GPU durable 和 preview 槽位，不根据 GPU 名称猜测 |

Gateway 的节点清单是 0 到 N 个对象组成的 JSON 数组。以下只展示一个对象；增加节点时复制
对象并替换稳定身份、私网地址和槽位：

```json
[
  {
    "nodeKey": "compute-REPLACE",
    "baseUrl": "http://10.66.0.REPLACE:18080",
    "maxDurableSlots": 1,
    "maxPreviewSlots": 2,
    "maxCpuSlots": 1,
    "maxGpuSlots": 1,
    "maxCpuPreviewSlots": 2,
    "maxGpuPreviewSlots": 1,
    "enabled": false
  }
]
```

`max*Slots` 是声明式配置，每次 Gateway 启动都会重新应用。节点的 `active`、`draining`、
`disabled` 状态存于 Gateway PostgreSQL；已有节点不会因为清单中 `enabled:true` 而覆盖人工
设置的 draining/disabled 状态。清单中 `enabled:false` 会在重启时强制保持 disabled。

从旧版“Compose 内嵌节点 JSON”迁移时，必须先把**完全相同的现有数组**加入 mode-0600 的
`/etc/fractal-prod/vps.env`，再安装新模板并运行 `config --quiet`。不要在迁移时先填 `[]`，
也不要顺手改 nodeKey、地址或槽位；模板来源迁移与节点拓扑变更应分成两个可回滚步骤。

## 3. 安装前输入和主机要求

开始前准备：

- 生产域名及 DNS 控制权；
- 一台长期在线的 VPS，建议至少 4 vCPU、8 GiB RAM；当前 2 GiB 规格只适合受控小流量并
  必须配置 Swap 和严格内存上限；
- 每台 Compute 主机的稳定远程管理通道、NVIDIA 驱动、可用 GPU 和足够磁盘；
- VPS 到所有 Compute 节点的 WireGuard UDP 可达性；
- 支付宝生产应用资料；Studio AI 开启时还需供应商密钥；
- 一台可构建并测试 amd64、Next.js 和目标 CUDA 架构镜像的构建机。

后文 `install ops/production/...` 命令假定当前目录是经过审核的 release checkout。目标主机不
必保留完整仓库；可以先经 SSH/SCP 把非敏感模板放入临时 staging 目录，再执行同样的
`install`。VPS 上只复制模板和加载镜像，不执行应用构建。

VPS 安装 Docker Engine、Compose v2、WireGuard、Caddy、`curl`、`openssl` 和防火墙工具。
Compute 主机安装 Docker Engine 或已验证的 Snap Docker、WireGuard、NVIDIA 驱动和
NVIDIA Container Toolkit。先记录版本，不要在共享节点上为安装本项目而重启 Docker：

```bash
docker version
docker compose version
wg --version
nvidia-smi
docker info --format '{{json .Runtimes}}'
```

VPS 应配置至少 4 GiB Swap、`net.ipv4.ip_forward=1` 和 Docker bridge 到 `wg0` 的路由。
配置后使用只读命令确认，不要通过关闭主机防火墙来排障：

```bash
swapon --show
sysctl net.ipv4.ip_forward
ip -brief address show wg0
docker network inspect bridge
```

## 4. 构建和验证不可变镜像

构建前工作区必须干净，且已确认远端分支状态：

```bash
git status --short --branch
git fetch origin master
git rev-parse --short=12 HEAD
```

先按 [docs/testing.md](../../docs/testing.md) 运行受影响服务的测试。以下用
`RELEASE_SHA` 表示上一步的 12 位 Git SHA；实际执行时显式替换，不使用 `latest`。

构建控制面镜像：

```bash
docker build -t fractal-platform:RELEASE_SHA platform-backend
docker build -t fractal-compute-gateway:RELEASE_SHA compute-gateway
docker build -f frontend/Dockerfile.release -t fractal-frontend:RELEASE_SHA .
```

Compute 构建使用 `scripts/stage-cuda-toolkit.sh` 生成 gitignored BuildKit context：

```bash
CUDA_TOOLKIT_DIR=/usr/local/cuda-13.0 \
  ./scripts/stage-cuda-toolkit.sh .cuda-toolkit
docker build \
  --build-context cuda-toolkit=.cuda-toolkit \
  --build-arg CUDA_ARCHITECTURES=89 \
  -f backend/Dockerfile \
  -t fractal-compute:RELEASE_SHA-sm89 .
```

Pascal `sm_61` 必须使用 CUDA 12.x（当前已验证 12.8）；CUDA 13 已移除 Pascal offline
compilation。Conda CUDA 也必须保留 staging 脚本创建的
`targets/x86_64-linux/nvvm -> ../../nvvm` 兼容链接：

```bash
CUDA_TOOLKIT_DIR=/path/to/cuda-12.8 \
  ./scripts/stage-cuda-toolkit.sh .cuda-toolkit
docker build \
  --build-context cuda-toolkit=.cuda-toolkit \
  --build-arg CUDA_ARCHITECTURES=61 \
  -f backend/Dockerfile \
  -t fractal-compute:RELEASE_SHA-sm61 .
```

在目标 GPU 上验证容器能够看到 CUDA，并跑真实 Compute 合同与至少一次真实 CUDA 请求。
请求参数中的 `engine=cuda` 不是执行证据；必须检查结果或 manifest 的 `actualEngine=cuda`、
`hardwareClass=gpu` 且没有 fallback。

通过 SSH 或 WireGuard 加密传输镜像。可使用 `docker save` 配合压缩流；不要使用公网裸 HTTP。
`gzip` 与 `gzip -d` 在目标主机上总是可用；只有当传输两端都确认安装了 `zstd` 时才改用 zstd
（当前 VPS 并未安装，直接用 zstd 会报 `zstd: 未找到命令`）：

```bash
docker save fractal-platform:RELEASE_SHA \
  fractal-compute-gateway:RELEASE_SHA \
  fractal-frontend:RELEASE_SHA \
  | gzip -1 \
  | ssh VPS_SSH_ALIAS 'gzip -d | docker load'
```

大文件建议先本地 `docker save | gzip` 成单个 tar.gz，`split -b 200M -d` 分片后用
`xargs -P4 scp` 并行传输，再在目标主机 `cat` 合并解压；每次传输后对比本机与目标机的
`sha256sum` 再加载。

每台 Compute 节点只需加载适合自身架构的镜像。传输后分别运行 `docker image inspect`，确认
tag 和 image ID 存在，再继续安装。

## 5. 配置 N 节点 WireGuard

在每台主机上以 `umask 077` 生成自己的 WireGuard 私钥；私钥只保留在该主机的
`/etc/wireguard/`，交换的只有公钥。不要把私钥粘贴到对话或 Git。

```bash
umask 077
wg genkey | tee /etc/wireguard/fractal-private.key \
  | wg pubkey > /etc/wireguard/fractal-public.key
chmod 0600 /etc/wireguard/fractal-private.key /etc/wireguard/fractal-public.key
```

管理员在主机本地把 private key 填入 `wg0.conf`；只复制 `fractal-public.key` 的内容到对端。

VPS 从 [wg0.vps.example](wg0.vps.example) 开始，每个 Compute 节点追加一个 `[Peer]`，且
`AllowedIPs` 只写该节点唯一的 `/32`。Compute 从
[wg0.compute.example](wg0.compute.example) 开始，只允许到 VPS 的 `REPLACE_VPS_WG_IP/32`。

```bash
install -m 0600 wg0.conf /etc/wireguard/wg0.conf
systemctl enable --now wg-quick@wg0
ip -brief address show wg0
wg show wg0
```

防火墙至少满足：

- VPS 公网只开放管理所需 SSH、`80/tcp`、`443/tcp` 和 `51820/udp`；
- VPS 的 PostgreSQL、Redis 和 Gateway 不发布宿主端口；Frontend、Platform API 和 MinIO
  只发布到 loopback；
- 每台 Compute 的 `18080/tcp` 只接受源 `REPLACE_VPS_WG_IP/32`，拒绝 LAN、公网和其他
  WireGuard peer；
- 不修改 Docker daemon 网络模式，不把 Gateway 改成 host network。

不同发行版的 nftables/firewalld/ufw 与 Docker 转发表行为不同，因此先查看现有规则，再用
主机原有防火墙体系添加精确规则。验收必须从 VPS、Compute 本机 LAN、其他 peer 和公网四个
方向实际连接，不能只看规则文本。

## 6. 安装每台 Compute 节点

仓库中的 [docker-compose.node1.yml](docker-compose.node1.yml) 虽保留历史文件名，但内容是
通用单节点模板；项目名、绑定地址和 runtime 都来自 env。普通 Docker 主机使用：

```bash
install -d -m 0755 /opt/fractal-compute-prod
install -d -m 0700 /etc/fractal-compute-prod
install -d -m 0750 /srv/fractal-compute-REPLACE-prod/runtime
install -m 0644 ops/production/docker-compose.node1.yml \
  /opt/fractal-compute-prod/docker-compose.node1.yml
install -m 0600 ops/production/compute.env.example \
  /etc/fractal-compute-prod/compute.env
sudoedit /etc/fractal-compute-prod/compute.env
```

这三个身份字段是必填项，模板故意没有 Node 1 默认值。从旧模板迁移的节点，必须先把它既有的 `COMPOSE_PROJECT_NAME`、`COMPUTE_BIND_IP`（唯一 WireGuard 地址）和现有 runtime
绝对路径原样补入 `/etc/<该项目>/compute.env`，经 `config --quiet` 验证后再更新模板。

确认 runtime 对镜像内 `fractal` 用户可写。先查看镜像 UID/GID，再只调整该 runtime 目录，
不要递归修改 `/srv`：

```bash
docker run --rm --entrypoint id fractal-compute:REPLACE_SHA-SM_REPLACE fractal
stat -c '%U:%G %a %n' /srv/fractal-compute-REPLACE-prod/runtime
```

若 owner 不匹配，读取镜像内 UID/GID 后只修正这个明确目录：

```bash
COMPUTE_UID=$(docker run --rm --entrypoint id fractal-compute:REPLACE_SHA-SM_REPLACE -u fractal)
COMPUTE_GID=$(docker run --rm --entrypoint id fractal-compute:REPLACE_SHA-SM_REPLACE -g fractal)
chown "$COMPUTE_UID:$COMPUTE_GID" /srv/fractal-compute-REPLACE-prod/runtime
```

渲染 Compose 并确认唯一目标后启动：

```bash
test "$(stat -c '%a %U:%G' /etc/fractal-compute-prod/compute.env)" = '600 root:root'
if awk -F= '!/^[[:space:]]*#/ && /=/ {v=tolower(substr($0,index($0,"=")+1)); if (v ~ /(replace|example)/) bad=1} END{exit bad ? 0 : 1}' \
  /etc/fractal-compute-prod/compute.env; then
  echo 'compute.env still contains example values' >&2
  exit 1
fi
docker compose ls
docker ps --filter label=com.docker.compose.project=fractal-compute-REPLACE-prod
docker compose -p fractal-compute-REPLACE-prod \
  --env-file /etc/fractal-compute-prod/compute.env \
  -f /opt/fractal-compute-prod/docker-compose.node1.yml config --quiet
docker compose -p fractal-compute-REPLACE-prod \
  --env-file /etc/fractal-compute-prod/compute.env \
  -f /opt/fractal-compute-prod/docker-compose.node1.yml up -d --no-build
```

普通 Docker 可安装
[fractal-compute-prod.service.example](fractal-compute-prod.service.example)。使用 Snap Docker 的
主机（如仓库当前部署的 Node 1）继续使用
[fractal-node1-prod.service.example](fractal-node1-prod.service.example)：Compose 的非敏感副本和
runtime 必须位于 `/var/snap/docker/common/`，env 仍由 systemd 从 `/etc` 注入。不要为规避 Snap
confinement 把密钥复制到 Snap 数据目录。

普通 Docker 节点安装 unit：

```bash
install -m 0644 ops/production/fractal-compute-prod.service.example \
  /etc/systemd/system/fractal-compute-prod.service
systemctl daemon-reload
systemctl enable --now wg-quick@wg0 fractal-compute-prod
systemctl status wg-quick@wg0 fractal-compute-prod --no-pager
```

Snap 主机必须先把模板路径替换为该节点当前的生产绝对路径（项目名、`/var/snap/docker/common/`
下的 Compose 与 runtime 路径），再安装对应专用 unit；不要把普通 Docker unit 与 Snap unit 混用：

```bash
install -m 0644 ops/production/fractal-node1-prod.service.example \
  /etc/systemd/system/REPLACE_COMPUTE_PROJECT.service
systemctl daemon-reload
systemctl enable --now wg-quick@wg0 REPLACE_COMPUTE_PROJECT
systemctl status wg-quick@wg0 REPLACE_COMPUTE_PROJECT --no-pager
```

验证单节点：

```bash
docker compose -p fractal-compute-REPLACE-prod \
  --env-file /etc/fractal-compute-prod/compute.env \
  -f /opt/fractal-compute-prod/docker-compose.node1.yml ps
curl --noproxy '*' -fsS http://10.66.0.REPLACE:18080/compute/v1/health
docker compose -p fractal-compute-REPLACE-prod \
  --env-file /etc/fractal-compute-prod/compute.env \
  -f /opt/fractal-compute-prod/docker-compose.node1.yml exec -T compute \
  sh -c 'curl -fsS -H "Authorization: Bearer $FSD_COMPUTE_SERVICE_KEY" http://127.0.0.1:18080/compute/v1/capabilities'
```

检查 renderer version、GPU 名称、compute capability、CUDA runtime 和可用 job。节点尚未加入
Gateway 时也应先完成真实 CUDA preview/durable render。

`FSD_STARTUP_BENCHMARK=quick` 会在启动时发布基础校准；已经单独完成并记录异步 benchmark 的
节点可经评审设为 `off`。共享或散热受限设备可设 `FSD_THERMAL_FRIENDLY=1`，但仍需通过实测
调整 `FSD_RENDER_THREADS` 和 Gateway 槽位，不能把 thermal 开关当作容量证明。

## 7. 准备 VPS 配置和持久目录

使用独立随机值建立以下边界：

| 配置 | 最低要求 |
|---|---|
| 两个 PostgreSQL password | 各自独立的高熵值 |
| `SESSION_SECRET` | 至少 32 字符，不以 `dev-` 开头 |
| Gateway service/admin/upstream key | 三个互不相同的高熵值；每个至少 32 字符 |
| MinIO root password | 独立高熵值 |
| MinIO KMS master key | 恰好 32 个随机字节的 base64，永久加密备份 |
| Alipay PEM | 官方生产应用私钥/公钥，不能使用 stub 或开发密钥 |
| AI provider 凭据（至少一个多模态模型） | 仅 `AI_ENABLED=true` 时需要。Studio AI 探索与上架文案都以图像为输入，因此活跃的 `AI_PROVIDER` 所用模型必须支持图像（如 `deepseek-v4-flash-vision-exp`、`Qwen3-VL-*-Instruct`），纯文本模型会直接失败；`siliconflow` 配 `SILICONFLOW_API_KEY`+`SILICONFLOW_MODEL`，`deepseek` 配 `DEEPSEEK_API_KEY`+`DEEPSEEK_BASE_URL`+`DEEPSEEK_MODEL`（建议 `AI_MAX_OUTPUT_TOKENS=2400`）。另一个 provider 可留空；key 只注入 API 服务 |

可在管理员的本地安全终端按需生成 32-byte hex/base64 值；每次输出只填一个对应字段，不在
shell history 中拼接完整 env：

```bash
umask 077
openssl rand -hex 32
openssl rand -base64 32
```

在 VPS 创建精确目录：

```bash
install -d -m 0755 /opt/fractal-prod
install -d -m 0700 /etc/fractal-prod /etc/fractal-prod/secrets
install -d -m 0750 \
  /srv/fractal-prod/postgres \
  /srv/fractal-prod/gateway-postgres \
  /srv/fractal-prod/redis \
  /srv/fractal-prod/minio
install -m 0644 ops/production/docker-compose.vps.yml \
  /opt/fractal-prod/docker-compose.vps.yml
install -m 0600 ops/production/vps.env.example /etc/fractal-prod/vps.env
sudoedit /etc/fractal-prod/vps.env
```

生产 env 至少包含三个镜像 tag、两个数据库密码、三类 Gateway key、MinIO 凭据和 KMS key、
支付宝字段及完整节点 JSON。`COMPUTE_UPSTREAM_SERVICE_KEY` 必须与所有节点的
`FSD_COMPUTE_SERVICE_KEY` 相同。各信任边界的 key 不得互相复用。

一次性生成并永久备份 MinIO KMS master key，格式为
`fractal-prod:<base64-32-byte-key>`。随意删除或轮换它会使已有 AES256 对象不可读。支付宝
PEM 文件放到固定路径：

```bash
install -m 0600 alipay_app_private.pem \
  /etc/fractal-prod/secrets/alipay_app_private.pem
install -m 0600 alipay_public.pem \
  /etc/fractal-prod/secrets/alipay_public.pem
stat -c '%a %U:%G %n' /etc/fractal-prod/vps.env /etc/fractal-prod/secrets/*.pem
```

Studio AI 初装建议保持 `AI_ENABLED=false`。非 AI 功能验收完成后再按
[ai-assistant.md](ai-assistant.md) 注入 API key 并单独启用；开启但缺 key 会故意阻止 API
启动。

## 8. 安装和启动 VPS 控制面

先确认所有不可变镜像已经加载，Compose 不需要构建或拉取：

```bash
test "$(stat -c '%a %U:%G' /etc/fractal-prod/vps.env)" = '600 root:root'
if awk -F= '!/^[[:space:]]*#/ && /=/ {v=tolower(substr($0,index($0,"=")+1)); if (v ~ /replace/) bad=1} END{exit bad ? 0 : 1}' \
  /etc/fractal-prod/vps.env; then
  echo 'vps.env still contains placeholder values' >&2
  exit 1
fi
# The VPS does not install jq; validate the JSON array with Python instead.
python3 - <<'PY'
import json
for raw in open('/etc/fractal-prod/vps.env'):
    if raw.startswith('COMPUTE_GATEWAY_BOOTSTRAP_NODES_JSON='):
        value = raw.split('=', 1)[1].strip()
        array = json.loads(value)
        assert isinstance(array, list) and array, 'bootstrap node array must be a non-empty JSON array'
        print('bootstrap nodes:', [n['nodeKey'] for n in array])
        break
else:
    raise SystemExit('COMPUTE_GATEWAY_BOOTSTRAP_NODES_JSON is missing')
PY
docker image inspect \
  fractal-platform:REPLACE_SHA \
  fractal-compute-gateway:REPLACE_SHA \
  fractal-frontend:REPLACE_SHA
docker compose -p fractal-prod \
  --env-file /etc/fractal-prod/vps.env \
  -f /opt/fractal-prod/docker-compose.vps.yml config --quiet
docker compose -p fractal-prod \
  --env-file /etc/fractal-prod/vps.env \
  -f /opt/fractal-prod/docker-compose.vps.yml config --images
```

将 [Caddyfile.example](Caddyfile.example) 合并到主机实际 Caddyfile。主机配置可能还包含其他
站点，不能直接覆盖。验证后才 reload：

```bash
caddy validate --config /etc/caddy/Caddyfile
systemctl reload caddy
```

首次安装尚未运行 Caddy 时，用 `systemctl enable --now caddy` 代替 reload，并再次检查
`systemctl status caddy --no-pager`。

启动完整控制面。`migrate`、`gateway-migrate` 和 `minio-init` 是预期成功退出的一次性服务：

```bash
docker compose ls
docker ps --filter label=com.docker.compose.project=fractal-prod
docker compose -p fractal-prod \
  --env-file /etc/fractal-prod/vps.env \
  -f /opt/fractal-prod/docker-compose.vps.yml up -d --no-build
docker compose -p fractal-prod \
  --env-file /etc/fractal-prod/vps.env \
  -f /opt/fractal-prod/docker-compose.vps.yml ps -a
```

三个一次性服务必须为 `Exited (0)`；长期服务必须 running，带 healthcheck 的服务必须
healthy。安装 [fractal-prod.service.example](fractal-prod.service.example) 后确认启动顺序是
`network -> wg0 -> Docker -> fractal-prod`。

```bash
install -m 0644 ops/production/fractal-prod.service.example \
  /etc/systemd/system/fractal-prod.service
systemctl daemon-reload
systemctl enable --now fractal-prod
systemctl status wg-quick@wg0 docker fractal-prod caddy --no-pager
```

不要只看 env 中的目标 tag。核对每个实际容器的 `.Config.Image`，尤其 API、worker 和
preview-worker 必须使用同一 Platform release：

```bash
docker ps -a --filter label=com.docker.compose.project=fractal-prod \
  --format '{{.Names}}|{{.Image}}|{{.Status}}'
```

若目标和实际 tag 不一致，说明只重建了部分服务；先查明原因，再对完整 `fractal-prod` 执行
一次显式 `up -d --no-build`。不要通过删除容器或 volume 强行“修复”。

## 9. 安全加入、扩容和退役 Compute 节点

### 9.1 加入节点

1. 完成节点本机 health、capabilities、CUDA 请求和端口隔离。
2. 在 `/etc/fractal-prod/vps.env` 的完整 JSON 数组中加入节点，先设 `enabled:false`。
3. `config --quiet` 后只重建 Gateway，使节点以 disabled 身份进入数据库。
4. 从 Gateway 容器直接请求该节点 health/capabilities，确认 bridge -> WireGuard 路径。
5. 把 env 中该节点改为 `enabled:true`，再次校验并重建 Gateway。
6. 对已有 disabled 节点调用 Gateway admin `activate`；探测成功后才会变成 active。
7. 做真实 preview 和 durable render，确认调度节点、CUDA 证据、manifest、VPS MinIO 摄取和
   最终下载。

更新 Gateway：

```bash
docker compose -p fractal-prod \
  --env-file /etc/fractal-prod/vps.env \
  -f /opt/fractal-prod/docker-compose.vps.yml config --quiet
docker compose -p fractal-prod \
  --env-file /etc/fractal-prod/vps.env \
  -f /opt/fractal-prod/docker-compose.vps.yml up -d --no-build compute-gateway
```

从 Gateway 自身读取节点视图，不在宿主命令行展开 admin key：

```bash
docker compose -p fractal-prod \
  --env-file /etc/fractal-prod/vps.env \
  -f /opt/fractal-prod/docker-compose.vps.yml exec -T compute-gateway python - <<'PY'
import json
import os
import urllib.request

request = urllib.request.Request(
    "http://127.0.0.1:8080/internal/v1/nodes",
    headers={"Authorization": "Bearer " + os.environ["COMPUTE_GATEWAY_ADMIN_KEY"]},
)
with urllib.request.urlopen(request, timeout=5) as response:
    print(json.dumps(json.load(response), ensure_ascii=False, indent=2))
PY
```

确认 env 中该节点已是 `enabled:true` 后，激活一个已验证的 disabled 节点：

```bash
TARGET_NODE_KEY=compute-REPLACE
docker compose -p fractal-prod \
  --env-file /etc/fractal-prod/vps.env \
  -f /opt/fractal-prod/docker-compose.vps.yml exec -T \
  -e TARGET_NODE_KEY="$TARGET_NODE_KEY" compute-gateway python - <<'PY'
import json
import os
import urllib.request

node_key = os.environ["TARGET_NODE_KEY"]
request = urllib.request.Request(
    f"http://127.0.0.1:8080/internal/v1/nodes/{node_key}/activate",
    data=b"",
    method="POST",
    headers={"Authorization": "Bearer " + os.environ["COMPUTE_GATEWAY_ADMIN_KEY"]},
)
with urllib.request.urlopen(request, timeout=10) as response:
    print(json.dumps(json.load(response), ensure_ascii=False, indent=2))
PY
```

同一路径的 `drain` 和 `disable` 是有状态生产操作，只有在获批退役/维护流程中才能使用。

节点清单不是“当前在线节点列表”，而是声明式资产清单。临时离线节点仍应保留；健康探测会在
其恢复后自动重新加入调度，无需重启 Gateway。

### 9.2 调整槽位

先跑节点 benchmark 和并发压力测试，再更新 JSON 中该节点的 CPU/GPU durable/preview
槽位。重启 Gateway 会应用新值。不要只通过 admin API 临时改槽位，否则下一次启动会被
声明式清单覆盖。

### 9.3 排空或退役

1. 先将节点设为 `draining`，阻止新任务进入。
2. 等待 durable 和 preview 使用槽位归零，并确认无尚未摄取的 artifact。
3. 将节点设为 `disabled`。
4. 把 env 清单中的该项设为 `enabled:false`，保留稳定 `nodeKey` 和地址记录。
5. 只有确认无恢复需要后，才停止该节点的精确 Compute Compose 项目。

仅从 JSON 删除对象不会删除 Gateway 数据库中的节点，也不会自动将它 disabled；因此不能把
“从文件删掉”当作退役操作。不要删除 Gateway 节点记录或 Compute runtime 来处理普通下线。

## 10. 上线验收

### 10.1 静态和服务检查

- Frontend production build、TypeScript/E2E、Platform tests、Gateway tests 和 Compute 合同
  测试通过；
- VPS 和每个节点的 Compose `config --quiet` 通过；
- 所有长期容器运行，一次性任务 `Exited (0)`；
- VPS 磁盘低于 75% 警戒线，日志轮转和 Swap 正常；
- env、WireGuard、支付宝和 KMS 文件权限为 `0600`。

### 10.2 公网与路由

```bash
curl --noproxy '*' -fsS -o /dev/null -w '%{http_code}\n' https://REPLACE_PRODUCTION_ORIGIN/
curl --noproxy '*' -fsS https://REPLACE_PRODUCTION_ORIGIN/platform/healthz
curl --noproxy '*' -sS -o /dev/null -w '%{http_code} %{redirect_url}\n' \
  'https://REPLACE_WWW_ORIGIN/install-probe?x=1'
```

确认 `/platform/*` 只进入 Platform API；当前签名下载使用保留完整路径的
`/fractal-platform/*` 进入 VPS MinIO。若主机仍保留 `/storage/*` 兼容路由，也必须确认它不
指向开发存储。开发域名只能进入独立开发项目。

### 10.3 多节点真实请求

- 从 Gateway 容器逐个请求所有节点的 health 和带鉴权 capabilities；
- 验证每个 active 节点的 renderer、GPU、compute capability 和 job 支持矩阵；
- 做一次真实 CUDA preview；
- 做至少一次 durable render，检查 sticky node、manifest、size/SHA-256、MinIO 对象和下载；
- 多节点时提交足够并发但受控的任务，证明任务能落到不同兼容节点；
- 私有对象未经授权不可下载，签名 URL 不泄露内部 MinIO 地址。

### 10.4 故障和恢复

故障演练会影响生产，必须先获得当次授权并写下恢复命令。演练矩阵至少覆盖单个节点离线、全部
节点离线、Gateway 重启和节点恢复。全部节点离线时，登录、会员、支付、市场、资产和已有下载
仍可用；新计算明确返回 503 且不创建任务、不扣额度。恢复节点后应由探测自动激活。

共享 Node 2 不再做主机重启演练。需要验证自恢复时，只能在维护窗口操作明确的 Fractal
Compute 容器，并不得触碰其他服务。

## 11. 日常发布

1. 记录当前 env、实际容器 tag、数据库迁移版本和回滚 tag。
2. 备份两个 PostgreSQL、MinIO、`/etc/fractal-prod`、Caddy 和 WireGuard 配置。
3. 在构建机完成测试并构建 Git-SHA 镜像；Compute 按目标架构分别构建。
4. 经 SSH/WireGuard 传输并 `docker load`，在目标机确认 image ID。
5. 一次性更新 `/etc/fractal-prod/vps.env` 的全部目标 tag；不要只更新 API。
6. 运行 `config --quiet` 和 `config --images`。
7. 对完整项目执行 `up -d --no-build`，确认迁移任务成功。
8. 比较目标 tag 与每个实际容器 tag，避免 worker/preview-worker 漂移。
9. 执行公网、Gateway、多节点、真实 CUDA、摄取和下载检查。
10. 至少保留上一版镜像、旧数据卷和配置备份 7 天。

应用回滚只把 env 中的 Frontend、Platform、Gateway tag 改回上一版，再完整执行
`up -d --no-build`。数据库一旦在新版本接受写入，不得指回已经分叉的旧数据库；需要用向前
兼容修复迁移。AI 可独立以 `AI_ENABLED=false` 回滚，不影响其他功能。

## 12. 数据备份与迁移

权威备份至少包含：

- Platform PostgreSQL 的一致性 dump；
- Gateway PostgreSQL 的一致性 dump；
- MinIO 全量对象及抽样 SHA-256；
- `/etc/fractal-prod`、Caddy、WireGuard 和所有 Compute env/systemd 配置的加密备份；
- 永久不丢失的 `MINIO_KMS_SECRET_KEY`。

跨主机迁移先预同步 MinIO，再进入 5–10 分钟停写窗口，完成两个数据库 dump/restore、对象
最终增量、表计数和抽样哈希，最后运行迁移并验收。备份只有在隔离环境成功恢复后才算有效。

Compute runtime 不代替上述备份。普通发布不得清空 runtime；它仍可能包含 Gateway 尚需按
原节点亲和读取的 run 和 artifact。

## 13. 常见故障

### Gateway health 200，但 capabilities/preview 为 503

这是零健康兼容节点或槽位耗尽时的预期 fail-closed 行为。检查 admin 节点视图、probe 时间、
节点 state、capabilities 兼容性和 CPU/GPU 槽位，不要重置数据库。

### Gateway migration报数据库密码错误

常见原因是持久 PostgreSQL volume 初始化时使用的密码与当前 env 不一致。生产环境先确认
凭据来源和数据库角色，再做受控密码修复；不得删除 volume。开发 volume 也应显式修复或新建
一个明确命名的可丢弃项目，不能用全局 prune。

### 容器 healthy，但 Gateway 到节点失败

本机 health 只证明容器内探针成功。依次检查 `wg0`、VPS 到节点地址、Docker bridge 路由、
转发和节点防火墙，并从 Gateway 容器发出真实请求。

### Pascal 节点报告 no kernel image

确认镜像是 CUDA 12.x 编译的 `sm_61`，不是其它节点（如 Node 1）的 `sm_89` 镜像；检查 capabilities 中
compiled/runtime 和 compute capability。不要用 CUDA 13 重试 Pascal offline build。

### Snap Docker 看不到 `/opt` 或 runtime

把非敏感 Compose 副本和 runtime 放到 `/var/snap/docker/common/`，由 systemd 从 `/etc`
加载 env。不要复制密钥到 Snap 数据目录，也不要改用生产/开发共享 volume。

### env tag 已更新，但 worker 仍运行旧镜像

说明发布只重建了部分服务。比较 `compose config` 的目标镜像和 `docker inspect` 的实际镜像，
然后对完整、明确的 `fractal-prod` 项目执行 `up -d --no-build` 并重新验收。

## 14. 完成定义

安装或扩容只有在以下条件全部满足时完成：模板和 runbook 与实际一致；敏感内容未入库；自动化
测试通过；每个节点网络隔离通过；真实 CUDA preview 和 durable render 通过；artifact 已摄取
到 VPS MinIO 并可授权下载；零节点行为、恢复和回滚已在获批窗口验证；备份可恢复；所有未测项
在当次发布记录或部署机本地的部署状态快照中明确列出。容器 running 或域名能打开不等于完成。

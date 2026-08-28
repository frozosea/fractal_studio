# 历史文档：云端 Platform + 双 Compute 节点部署计划

> **状态：迁移前方案记录，不可作为当前操作手册。** 文中的单节点限制、OSS/CDN 目标、
> 双节点固定拓扑、容量不足排队语义、机器资源和工作量估算均保留当时背景，部分已经过时。
> 当前实现是 VPS 控制面 + 0..N 私有 Compute 节点，容量不足 fail closed；请使用
> [多节点生产安装与部署手册](../ops/production/INSTALL.md) 和
> [实际部署状态](../ops/production/STATUS.md)。

本文规划把长期在线的业务控制面部署到 `admin@fractal.kevin0412.top` 所在 VPS，
把 C/C++ Compute 保留在两台用户侧机器：

- 主节点：i9-14900K + RTX 4090；
- 辅助节点：i5-8300H + GTX 1050 Laptop。

计划以持续执行时长和 token 区间估算工作量，不使用人日。估算包含代码检查、修改、
测试和部署验证；镜像下载、CUDA 编译、对象上传和 DNS 生效等纯等待时间单独说明。

## 1. 结论

该拓扑可行，并且比“全部服务继续通过 FRP 暴露”或“全部服务迁入当前 2 核 2G VPS”更合理：

- 登录、市场、支付、管理和任务状态不再依赖测试机持续在线；
- 支付宝回调落在稳定公网主机上；
- RTX 4090 和 14900K 继续承担主要计算；
- GTX 1050 Laptop 提供低优先级容量和有限故障降级；
- 市场图片和导出文件由 OSS/CDN 直接分发，不占 VPS 的低速公网出口。

但仅迁移 Python 和 TypeScript **不会自动解决图片慢**。现场测得 VPS 对外有效吞吐约
3.4 Mbit/s，一张约 1 MiB 的预览需要约 2.4 秒。若仍由 VPS MinIO/Caddy 输出图片，
迁移后吞吐上限基本不变。对象存储和 CDN 是本计划的必要部分，而不是可选优化。

## 2. 目标拓扑

```text
Browser
  |
  v
VPS
  +-- Caddy / HTTPS
  +-- Next.js frontend
  +-- FastAPI API
  +-- PostgreSQL
  +-- Redis
  +-- Outbox / payment / media worker
  +-- Compute node router
  |
  +---- private outbound tunnel ----> Compute A: 14900K + RTX 4090
  |
  +---- private outbound tunnel ----> Compute B: i5-8300H + GTX 1050 Laptop
  |
  +---- S3 API ---------------------> OSS/CDN ----> Browser
```

两台 Compute 节点都主动向 VPS 建立隧道。Compute HTTP 服务不得直接暴露到公网；
VPS 只通过回环地址、受控 overlay 网络或具备服务鉴权的私有端口访问节点。

`kevin0412.top` 当前只有公网 IPv6，而现有 VPS 没有 IPv6 默认路由，因此不能依赖
VPS 直接访问该域名。可选传输按优先顺序为：

1. Tailscale/WireGuard overlay；
2. FRP `stcp` 或仅绑定 VPS 回环地址的受控 TCP proxy；
3. 不接受把 Compute 端口绑定到公网 `0.0.0.0`。

## 3. 现状与约束

### 3.1 VPS

现场采样的 VPS 是 `ecs.e-c1m1.large`：2 vCPU、2 GiB 内存、无 Swap、40 GiB
系统盘。它适合充当网关和小规模业务控制面，但不适合运行 C++ Compute，也不适合在机器上
执行 Next/CUDA 镜像构建。

在不运行 MinIO、Compute 和支付宝 stub，且镜像在其他机器预先构建的前提下，2 GiB 可以
用于第一阶段小流量验证。正式对外建议升级到至少 4 vCPU / 8 GiB。若暂不升级，必须设置
容器内存限制、增加受控 Swap、保留健康检查和一键回滚。

### 3.2 对象和数据库规模

规划时的现场数据约为：

- MinIO：865 MiB；
- PostgreSQL：72 MiB；
- 测试机根分区仅剩约 32 GiB，使用率 97%。

数据库迁移量很小；对象迁移可先做全量同步，再在切换窗口做增量校验。测试机磁盘容量是近期
故障风险，应通过迁移对象、清理构建缓存和设置运行目录保留策略解决。

### 3.3 当前 Platform 只支持一个 Compute 节点

当前实现不能仅通过增加第二个 `COMPUTE_BASE_URL` 完成负载均衡：

- `ComputeClient` 只读取一个 base URL 和一个 service key；
- `render_jobs` 只持久化 `compute_run_id`，没有持久化 `compute_node_id`；
- submit、poll、cancel、manifest 和 artifact stream 都默认访问同一节点；
- Artifact ID 只在产生它的节点内有效；
- 项目合同已经要求保存 `compute_node_id`，但数据库和生产代码尚未落实。

任务提交到某个节点后，所有后续操作必须保持节点亲和。不能在轮询或下载阶段重新选择节点。

### 3.4 GTX 1050 需要独立构建

当前 `backend/Dockerfile` 使用 CUDA 13 runtime，并显式设置：

```text
CMAKE_CUDA_ARCHITECTURES=89
```

这只面向 RTX 4090 的 Ada `sm_89`。GTX 1050 Laptop 属于 Pascal `sm_61`；CUDA 13
已经移除 Pascal 离线编译支持。因此辅助节点需要独立的 CUDA 12.x 构建产物，例如：

```text
CMAKE_CUDA_ARCHITECTURES=61
```

不得把 4090 镜像直接复制到 GTX 1050 后宣称 CUDA 可用。构建完成后必须通过 capabilities、
真实 CUDA 任务和结果差分测试验证，而不能只检查 `nvidia-smi`。

### 3.5 Preview 和 Artifact 的网络成本

当前同步 Preview 从 C++ 获取 RGBA8，再由 Python 编码 PNG。1024x1024 RGBA 帧约为 4 MiB，
跨隧道传输会放大交互延迟。计划中应让 Compute 返回压缩 PNG，或者增加兼容的压缩 Preview
合同，Platform 只校验并转发结果。

持久任务完成后，当前 Worker 会把 Compute Artifact 下载到 VPS 临时文件，再上传 S3。
第一阶段可保留该路径，因为它具备大小和 SHA-256 校验；后续大型视频可改成 Compute 使用
一次性预签名 PUT 直传 OSS，避免文件经 VPS 中转两次。

## 4. 节点职责和调度策略

### 4.1 Compute A：14900K + RTX 4090

默认承担：

- CUDA map/Julia/transition 图片；
- 视频、3D、体素和高分辨率导出；
- 需要较大显存的任务；
- MPFR、特殊点和扰动理论等 CPU/高精度工作；
- 显式要求 `cuda`、`hybrid`、`fp128` 或 `mpfr` 的任务。

### 4.2 Compute B：i5-8300H + GTX 1050 Laptop

初始只承担：

- 有界低分辨率 Preview；
- 低优先级、小显存 CUDA 任务；
- 4090 节点不可用时的有限降级；
- 跨 GPU 结果一致性抽样。

该节点不得与 4090 等权轮询。GTX 1050 的显存、吞吐和笔记本散热都显著弱于 4090，
i5-8300H 也不适合接管 14900K 的 MPFR 重任务。正式权重必须由同一套基准实测决定。

### 4.3 调度输入

节点选择至少考虑：

- 健康状态和最后心跳；
- capabilities 中的 job kind、engine、scalar 和 CUDA compute capability；
- 可用显存和任务估算显存；
- 节点正在运行/排队的任务数；
- Preview 与 Durable Job 的不同优先级；
- 用户显式 engine/scalar 要求；
- 节点 drain/maintenance 状态；
- 最近失败率、温度降频或超时情况。

第一版不做运行中任务迁移。节点掉线时，已提交的任务保持原节点亲和并等待恢复或超时失败；
尚未提交的任务才允许选择其他节点。这能避免至少一次投递下产生重复计算和错误 Artifact。

## 5. 分阶段实施

### 阶段 0：基线和准入门

1. 为两台 Compute 主机建立 SSH/overlay 访问，但不开放公网 Compute 端口。
2. 记录 CPU、内存、GPU、显存、CUDA Driver、磁盘和上下行吞吐。
3. 在 4090 上固化当前 benchmark 结果。
4. 在 1050 上验证 CUDA 12.x、`sm_61`、所有目标 kernel 和驱动兼容性。
5. 使用相同 recipe 集合比较 CPU/CUDA、结果 hash、P50/P95 和峰值显存。

准入条件：若 1050 不能稳定通过 CUDA 差分测试，它只注册为 CPU/备用节点，不阻塞主系统迁移。

### 阶段 1：对象存储和图片分发

1. 创建 OSS bucket，配置私有 master、公开衍生图、服务端加密和生命周期规则。
2. 使用现有 S3 抽象接入 OSS endpoint；验证签名 URL、CORS、Range 和缓存头。
3. 全量同步现有约 865 MiB 对象并核对数量、大小和 hash。
4. 将市场 thumbnail/watermarked preview 置于 OSS/CDN 路径。
5. 保持旧 MinIO 只读，直到回滚窗口结束。

验收：市场图片请求不再经过 VPS 的低速公网出口；已购买 master 仍保持鉴权和短时签名访问。

### 阶段 2：VPS Production 控制面

1. 补齐 production 环境文件和 secrets 管理，禁止提交密钥。
2. 在外部构建 Next/FastAPI 镜像，VPS 只拉取并启动。
3. 部署 Caddy、Frontend、API、Worker、PostgreSQL 和 Redis。
4. 先用 staging hostname 验证登录、CSRF、管理员隔离、市场、购买和支付宝沙箱。
5. 同步数据库，短暂停写后做最后一次 dump/restore 和行数校验。
6. 保持现有域名和支付宝 callback URL，只把 Caddy upstream 从 FRP 改到本机容器。

回滚只需恢复原 Caddy upstream，并重新启用旧 API/Frontend proxy，不修改用户浏览器地址。

### 阶段 3：4090 单节点闭环

1. 4090 Compute 通过私有隧道注册到 VPS。
2. VPS Worker 完成 capabilities、Preview、submit、poll、cancel、manifest 和 Artifact 摄取。
3. Preview 改为压缩结果传输，避免 4 MiB RGBA 帧跨网。
4. 对 PNG、视频、mesh、MPFR 和取消流程做端到端测试。
5. 模拟断网、节点重启和重复 Outbox 投递。

该阶段完成后即可切换主生产流量，不必等待 GTX 1050 和自动调度完成。

### 阶段 4：GTX 1050 辅助节点

1. 增加独立 Pascal/CUDA 12 构建定义，不影响 4090 CUDA 13 镜像。
2. 验证 `sm_61` kernel、运行时库、FFmpeg、MPFR 和 Artifact 合同。
3. 设置较低并发、显存上限、温度/掉电保护和节点 drain。
4. 只开放通过基准验收的 job kind/engine/scalar 能力。
5. 初期仅手动或静态路由轻任务，观察稳定性后再进入自动调度。

### 阶段 5：多节点持久化和调度

1. 新增 `compute_nodes` 配置/状态模型。
2. 数据库迁移为 `render_jobs` 增加不可变的 `compute_node_id` 亲和字段。
3. 将 Compute Client 改为按节点创建，并支持每节点独立 service key。
4. submit 时选择节点并原子保存 node ID；poll/cancel/manifest/artifact 只读取已保存节点。
5. Preview 使用能力和负载选择节点，但保持短时间粘性，避免视图连续操作来回切换。
6. 增加 heartbeat、drain、容量、失败退避和“全部节点不可用时排队”语义。
7. 管理端展示节点在线状态、队列、GPU/CPU、最近错误和实际执行 engine。

### 阶段 6：性能与可靠性收尾

1. 修复 Market 的 N+1 preview 查询和重复创建 S3 client 问题。
2. 对首页、Market、Preview、支付和导出分别建立 P50/P95 基线。
3. 增加节点断线、VPS 重启、OSS 暂时失败、重复回调和 Artifact 校验失败测试。
4. 配置数据库和对象存储备份、日志保留、磁盘告警和服务健康告警。
5. 完成 production smoke、压力测试和完整回滚演练。

## 6. 时长与 token 估算

下面是持续执行的区间，不是人日：

| 阶段 | 预计持续时长 | 预计 token |
|---|---:|---:|
| 基线、1050 准入和网络检查 | 1–2 小时 | 10k–20k |
| VPS production 控制面 | 3–5 小时 | 35k–60k |
| OSS/CDN 接入和数据迁移 | 2–4 小时 | 25k–45k |
| 4090 私有接入与单节点闭环 | 2–4 小时 | 25k–45k |
| 压缩 Preview 合同与实现 | 2–4 小时 | 25k–45k |
| GTX 1050 CUDA 12 / sm_61 构建与验证 | 3–6 小时 | 30k–60k |
| 多节点 schema、亲和、路由和健康调度 | 7–12 小时 | 80k–140k |
| 故障测试、监控、压力测试和回滚演练 | 4–7 小时 | 40k–70k |

组合估算：

- **可上线 MVP**：VPS 控制面 + OSS/CDN + 4090 单节点，约 8–15 小时、
  95k–175k token。
- **带 1050 静态备用**：约 14–22 小时、150k–240k token。
- **完整自动双节点生产调度**：约 24–40 小时、270k–430k token。

CUDA/依赖下载、镜像编译、865 MiB 对象上传和外部 DNS/CDN 生效可能额外增加 2–6 小时墙钟
等待，但通常只消耗少量 token。若 1050 的驱动、CUDA 12 或 kernel 兼容失败，排障时间另计；
该失败不得阻塞 4090 单节点 MVP 上线。

## 7. 上线验收

上线至少满足：

- VPS 重启后所有控制面服务自动恢复；
- 测试机关机时，登录、市场、已购内容和支付宝回调仍可工作；
- Compute 全部离线时，新任务保持 queued 或返回明确可重试状态，不影响支付和浏览；
- 每个 Durable Job 均保存 `compute_node_id` 和 `compute_run_id`；
- poll、cancel、manifest 和 artifact 始终访问原节点；
- 市场图片由 OSS/CDN 返回，不经过本地 MinIO/FRP；
- 4090 任务记录实际 CUDA 执行证据；
- 1050 只接收其 capabilities 和显存允许的任务；
- Artifact 在进入商业资产前校验 size、SHA-256 和节点亲和；
- 数据库、对象和 Caddy upstream 均有已演练的回滚路径。

## 8. 暂不纳入第一版

- 运行中任务跨节点迁移；
- GTX 1050 与 RTX 4090 等权负载均衡；
- Kubernetes 或复杂服务网格；
- Compute 直接暴露公网；
- 未经基准就根据 GPU 名称估算调度权重；
- 在 2 GiB VPS 上构建 Next.js 或 CUDA 镜像。

# Commercialization Implementation / 商业化实施记录

本文是 Fractal Studio 商业化重构的执行依据与进度事实来源。实施过程中每个功能只有在代码、自动测试和文档三者都完成后，才标记为 `completed`。

最后更新：2026-07-23

## Status Legend / 状态说明

- `pending`：尚未开始。
- `in_progress`：正在实现，尚未满足验收条件。
- `completed`：实现、测试和文档均已完成。
- `blocked`：存在已记录的外部阻塞。

## Architecture Decision / 架构决策

目标运行拓扑：

```text
Vue 3 frontend
  -> FastAPI Platform API
     -> PostgreSQL product state
     -> Redis quotas and rate limits
     -> PostgreSQL Outbox Worker
        -> private C++ Compute v1
        -> OSS / MinIO assets
```

边界约定：

1. C++ 只拥有分形数学、硬件执行、Compute run 和临时产物。
2. FastAPI 是商业浏览器唯一后端，拥有账户、配方、平台任务、资产、市场、交易和账本。
3. 迁移期间旧 `/api/*` 保持可用；新增 `/compute/v1/*` 供 Platform 私网调用。
4. 自定义公式使用安全 DSL/AST/字节码；生产环境禁止运行时 `g++ + dlopen`。
5. Orbit Program 统一承载单公式、周期序列和后续组合玩法；现有 axis transition 保持原数学语义。
6. 无法证明有限逃逸半径时，`certifiedRadius=null`，不得用有限阈值宣称数学逃逸。
7. 首发部署为阿里云单区、单 Compute 节点；协议和任务记录预留多节点扩展。

## Milestones / 里程碑

| ID | Milestone | Status | Acceptance |
|---|---|---|---|
| C0 | 基线与能力清单 | completed | 当前构建/测试通过；所有旧端点和能力形成单一映射。 |
| C1 | C++ Compute v1 | in_progress | 鉴权、capabilities、preview、run/status/cancel、manifest、artifact 闭环完成。 |
| C2 | 安全 DSL 与 Orbit Program | in_progress | DSL 无原生代码执行；单公式和周期序列接入通用 CPU 渲染；严格逃逸元数据完成。 |
| P0 | FastAPI 架构底座 | in_progress | Python 工程、迁移、ComputeClient、Outbox Worker 和真实渲染闭环完成。 |
| D0 | 本地部署底座 | in_progress | Compose 启动 API、Worker、PostgreSQL、Redis、MinIO、Compute 并通过健康检查。 |
| F0 | 前端双轨接入 | pending | 保持现有设计；Platform/legacy API 可按环境切换；现有页面无回归。 |
| M1 | 身份与工作室 | pending | 不透明会话、RBAC、不可变配方、配额、预览和渲染任务完成。 |
| M3 | 资产与媒体库 | pending | Compute manifest 校验、对象存储、衍生文件和受保护下载完成。 |
| M4 | 市场与许可证 | pending | 草稿、发布快照、目录、许可证完成。 |
| M5 | 支付与权益 | pending | 支付宝通知、不可变订单、权益和下载闭环完成。 |
| M6 | 账本与结算 | pending | 只追加账本和人工打款流程完成；API 打款保持关闭。 |

### Progress Snapshot / 进度快照

以下百分比按本文可验收事项粗略计数，只表示工程完成度，不表示工期：

| Scope | Progress | Current boundary |
|---|---:|---|
| Compute 底座（C0–C2） | 约 92% | 2D/Julia/HS/ln-map 安全 DSL、typed AST 与周期序列已可用；zoom 视频传播未完成。 |
| Platform/部署底座（P0–D0） | 约 68% | API/Worker/Outbox 代码完成；真实 PostgreSQL/完整 Compose E2E 受 Docker 权限限制。 |
| 前端双轨（F0） | 0% | 尚未开始拆分前端 API 与页面。 |
| 商业模块（M1–M6） | 0% | 身份、资产、市场、支付和账本尚未开始。 |
| 完整商业化路线 | 约 29% | 已完成可运行的 Compute/Platform 技术底座，产品商业闭环尚未进入。 |

## Current Work / 当前工作

### C0 — 基线与能力清单

- [x] 确认工作树基线；用户提供的 `platform-backend-spec.zh.pdf` 尚未纳入版本控制。
- [x] 阅读现有架构、二维、三维、视频、测试和 feature-status 文档。
- [x] 枚举现有 C++ HTTP 入口和前端实际调用面。
- [x] 完成 Release 构建和现有 CTest 基线。
- [x] 建立 Compute v1 能力注册与版本化 `kind` 列表。
- [x] 在 `routes_compute_v1.cpp` 固化旧 API 到 Compute v1 `kind` 的完整映射。

### C1 — C++ Compute v1

- [x] 私网 Bearer 服务密钥与常量时间比较。
- [x] 版本化错误包络和 `422 UNSUPPORTED_CAPABILITY`。
- [x] 健康、能力和同步预览接口骨架；map preview 返回原始二进制帧。
- [x] 统一 run 创建、状态和取消适配；原生后台任务保持异步，同步 legacy job 暂在 Worker 调用内完成。
- [x] 统一 manifest、相对路径约束、MIME、大小和 SHA-256。
- [x] 私有 artifact 流式读取与 Range。
- [x] 旧 `/api/*` 双轨兼容和 `FSD_ENABLE_LEGACY_API` 商业关闭开关。
- [x] Compute v1 HTTP/鉴权/manifest 自动测试。
- [x] `idempotencyKey` 持久响应缓存；重复 Outbox 提交返回同一 Compute run。
- [ ] 将仍同步执行的 mesh/ln-map legacy job 改为统一后台 run。

### C2 — 安全 DSL 与 Orbit Program

- [x] Pratt parser、AST、规范化序列化和稳定 SHA-256 hash。
- [x] AST 实数/复数静态类型推导与算术提升；参数声明类型参与规范化 hash，解释器仍使用统一复数寄存器保存数值。
- [x] 4 KiB 源码、256 节点、32 层、16 参数和序列预算限制。
- [x] 编译错误码、字符位置和无堆分配的定长栈字节码解释器。
- [x] `z`、`c`、只读 `n`、参数、`i/pi/e` 和既有安全函数。
- [x] Formula/Sequence IR、正整数 `span`、确定性循环日程和稳定 hash。
- [x] Mandelbrot/Burning Ship 一步一换及不同 `span` 的通用执行基础。
- [x] `certified_finite`、`no_finite_bound`、`unverified` 三态模型；当前证明器产出前两类中的 `certified_finite` 与默认 `unverified`，`no_finite_bound` 保留给后续已证明反例节点。
- [x] 无证书公式不使用有限模长阈值；跑满或进入 `numerically_diverged/indeterminate`，不冒充数学逃逸。
- [x] Julia 常量使用满足 `R²-|c|>R` 的逐请求安全半径；普通参数平面保留已认证二次族的半径 2 语义。
- [x] 2D/Julia image、raw field、同步 preview 和异步 map run 接入；实际 engine/scalar 固定报告 OpenMP/fp64。
- [x] legacy `g++ + dlopen` 默认关闭，且只有 legacy API 与 compiler 两个开关都显式为真才允许；商业模式无法加载历史 `.so`。
- [x] Orbit Program 传播到 HS field/mesh；escape、min/max abs、envelope 和 recurrence 走确定性 fp64/OpenMP 通用路径。
- [x] Orbit Program 传播到 ln-map 的 escape 与全部 mapped color modes；单公式与旧路径执行 HTTP PNG hash parity。
- [ ] Orbit Program 传播到 zoom 和其他派生视频；axis transition 视频不接受普通 Orbit Program。
- [ ] `weighted_schedule`、`output_blend`、参数曲线和动画；axis transition 继续保持独立顶层数学。

### P0 — FastAPI 架构底座

- [x] Python 3.12 项目、配置、日志、request ID 和健康检查。
- [x] SQLAlchemy async 模型与 Alembic 初始迁移。
- [x] `render_jobs`、`quota_reservations`、`outbox_events`。
- [x] 强类型 ComputeClient 和结构化错误映射。
- [x] `SKIP LOCKED` 租约、至少一次投递和指数退避。
- [x] submit/poll/cancel/manifest 校验代码闭环。
- [x] development/test 固定主体；生产配置拒绝启用未认证 Studio 路由。
- [x] FastAPI 到真实 C++ Compute 的 64×64 RGBA preview 冒烟。
- [ ] 在真实 PostgreSQL 上执行迁移并验证 Outbox render/cancel E2E。
- [ ] 接入 Redis 原子配额；当前底座只有 PostgreSQL reservation。

### D0/F0 — 部署与前端迁移

- [x] 本地 Compose、迁移 one-shot 服务和健康依赖配置。
- [ ] 实际启动完整 Compose；当前执行用户无 Docker daemon 权限。
- [ ] MinIO/S3 与生产 OSS 适配器边界。
- [ ] 拆分 Platform API、legacy API 和领域 DTO。
- [ ] 环境开关与 Cookie 请求策略。
- [ ] 保留设计 token、导航、画布、3D 查看器和响应式行为。

## Verification Log / 验证记录

| Date | Scope | Command / Evidence | Result |
|---|---|---|---|
| 2026-07-23 | Initial baseline | `cmake -S backend -B runtime/build -DCMAKE_BUILD_TYPE=Release && cmake --build runtime/build -j2` | passed |
| 2026-07-23 | Existing C++ suite | `ctest --test-dir runtime/build --output-on-failure` | 6/6 passed before Compute v1 changes |
| 2026-07-23 | Compute v1 compile | Release build with OpenSSL-backed SHA-256 and new private routes | passed |
| 2026-07-23 | Compute v1 HTTP contract | `compute_v1_http_smoke` covers auth, capabilities, RGBA preview, background run, manifest and artifact streaming | passed; full CTest 7/7 |
| 2026-07-23 | Compute idempotency | `compute_v1_http_smoke` repeats the same key and asserts the same run ID | passed; full CTest 7/7 |
| 2026-07-23 | Platform unit tests | isolated Python 3.12 venv; `pytest -q platform-backend/tests` | 5 passed |
| 2026-07-23 | Platform migration | `alembic upgrade head --sql` using PostgreSQL dialect | passed; 73 lines generated |
| 2026-07-23 | Platform preview integration | Uvicorn -> Compute v1 -> OpenMP 64×64 RGBA; propagated engine/scalar/request headers | passed |
| 2026-07-23 | Compose validation | `docker compose -f docker-compose.dev.yml config -q` | passed; daemon start unavailable to current user |
| 2026-07-23 | Safe DSL/Orbit unit contract | `orbit_program_smoke` covers parser, constants/parameters, canonical hash, limits and M/B deterministic sequence | passed |
| 2026-07-23 | Strict escape regression | `compute_path_diff` covers certified escape, unverified run-to-limit and numerical-divergence classification | passed |
| 2026-07-23 | Orbit HTTP/run contract | `compute_v1_http_smoke` covers capabilities, sequence preview, raw strict field, compile errors, async manifest certificate and native compiler denial | passed; full CTest 8/8 |
| 2026-07-23 | HS Orbit parity/strictness | `hs_orbit_smoke` covers builtin parity, M/B sequence determinism and unverified run-to-limit | passed |
| 2026-07-23 | HS Compute run | `compute_v1_http_smoke` creates an HS mesh with Orbit sequence and verifies GLB/STL manifest entries and radius certificate | passed; full CTest 9/9 |
| 2026-07-23 | Typed Orbit AST | `orbit_program_smoke` distinguishes real/complex parameters in canonical hashes and verifies function result/promotion types | passed; full CTest 9/9 |
| 2026-07-23 | Output-blend escape counterexample | 50% Mandelbrot/Burning Ship complex-output DSL blend must report `certifiedRadius=null`; overflow remains numerical divergence, never escape | passed in `orbit_program_smoke` and `compute_path_diff` |
| 2026-07-23 | Real Compute HTTP matrix | backend subprocess + Bearer HTTP covers 400/401/422 envelopes, binary/JSON previews, async poll/cancel, manifest, artifact and Range 206 | passed in `compute_v1_http_smoke` |
| 2026-07-23 | Orbit ln-map HTTP parity | Compute run renders escape/hist_eq sequence strips; builtin Orbit PNG SHA-256 equals legacy builtin PNG | passed; full CTest 9/9 |

## Commit Log / 提交记录

| Commit | Scope |
|---|---|
| `b5ea58c` | 计划文档、Compute v1 鉴权/能力/run/manifest/artifact 契约及 HTTP CTest。 |
| `52b5594` | Compute v1 持久幂等响应缓存；重复 Platform 提交复用同一个 native run。 |
| `5e0987a` | FastAPI、PostgreSQL Outbox、ComputeClient/Worker、Alembic 与本地 Compose 底座。 |
| `20db668` | 安全 DSL 字节码、Orbit Formula/Sequence、严格逃逸分类及 2D/Julia Compute 接入。 |
| `7cd4def` | legacy 原生公式编译与 `.so` 加载默认关闭，并加入商业模式双开关保护。 |
| `d53a17d` | Orbit Formula/Sequence 扩展到 HS field/mesh，并增加严格逃逸和 Compute manifest 回归。 |
| `7c7c185` | DSL AST 增加 real/complex 类型推导、参数声明类型和规范化 hash 类型信息。 |
| `24434e9` | 固化 Mandelbrot/Burning Ship 各半输出组合无有限证书、不得误判逃逸的回归测试。 |
| `d0cbbdd` | 扩展真实 Compute HTTP 进程测试，覆盖能力拒绝、Range 和取消终态。 |
| `600b46b` | Orbit Program 接入 ln-map escape/mapped 路径、sidecar 证明信息和 HTTP parity。 |

## Delivery Rules / 交付规则

1. 每次状态变化都更新本文的里程碑、当前工作和验证记录。
2. 能力支持必须来自代码注册表；文档不能单独宣称 kernel 未实现的能力。
3. 不支持的组合显式失败，禁止静默替换 variant、Orbit Program 或数学语义。
4. 现有本地 SQLite 历史只读保留，不迁入商业 PostgreSQL。
5. `platform-backend-spec.zh.pdf` 是 M1–M7 领域与公共 API 的权威基线。

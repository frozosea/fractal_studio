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
| C1 | C++ Compute v1 | completed | 鉴权、capabilities、preview、全部现有持久产物的 run/status/cancel、manifest、artifact 与硬件证据闭环完成。 |
| C2 | 安全 DSL 与 Orbit Program | completed | DSL 无原生代码执行；单公式和周期序列接入约定管线；严格逃逸与 Orbit 复用一致性完成。 |
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
| Compute 底座（C0–C2） | 100% | 首个商业底座范围完成；weighted schedule、output blend 和参数曲线属于后续产品玩法批次。 |
| Platform/部署底座（P0–D0） | 约 70% | API/Worker/Outbox、Compute 产物流式完整性校验及对接文档完成；真实 PostgreSQL/完整 Compose E2E 受 Docker 权限限制。 |
| 前端双轨（F0） | 0% | 尚未开始拆分前端 API 与页面。 |
| 商业模块（M1–M6） | 0% | 身份、资产、市场、支付和账本尚未开始。 |
| 完整商业化路线 | 约 30% | 已完成可运行的 Compute/Platform 技术底座，产品商业闭环尚未进入。 |

## Current Work / 当前工作

### C0 — 基线与能力清单

- [x] 确认工作树基线；用户提供的 `platform-backend-spec.zh.pdf` 尚未纳入版本控制。
- [x] 阅读现有架构、二维、三维、视频、测试和 feature-status 文档。
- [x] 枚举现有 C++ HTTP 入口和前端实际调用面。
- [x] 完成 Release 构建和现有 CTest 基线。
- [x] 建立 Compute v1 单一能力注册表；由同一表生成版本化 persistent/preview `kind` 清单、入口校验和逐 job 的 variant profile、metric、engine、scalar、Orbit、输出格式兼容信息。
- [x] 在 `routes_compute_v1.cpp` 固化旧 API 到 Compute v1 `kind` 的完整映射。

### C1 — C++ Compute v1

- [x] 私网 Bearer 服务密钥与常量时间比较。
- [x] 版本化错误包络和 `422 UNSUPPORTED_CAPABILITY`。
- [x] 健康、能力和同步预览接口骨架；map preview 返回原始二进制帧。
- [x] 统一 run 创建、状态和取消适配；所有现有持久 Compute kind 均通过后台 run 执行，旧 `/api` 的同步响应按双轨兼容约定保留。
- [x] 统一 manifest、相对路径约束、MIME、大小和 SHA-256。
- [x] 私有 artifact 流式读取与 Range。
- [x] 旧 `/api/*` 双轨兼容和 `FSD_ENABLE_LEGACY_API` 商业关闭开关。
- [x] Compute v1 HTTP/鉴权/manifest 自动测试。
- [x] Compute v1 HTTP 合同迁移为按领域拆分的 pytest 套件；每个测试独立启动后端、独立创建 run/产物，原 smoke 仅保留最小核心链路语义。
- [x] capabilities 暴露 CPU/OpenMP/AVX/CUDA 编译与运行时快照；map run status/manifest 保存 kernel 完成点报告的实际 engine/scalar、硬件类别和回退状态。
- [x] `idempotencyKey` 持久响应缓存；重复 Outbox 提交返回同一 Compute run。
- [x] 幂等键绑定 kind/payload hash；同一 key 被不同请求复用时返回 `409 IDEMPOTENCY_CONFLICT`，不再误返回旧任务。
- [x] 注册表字段在进入旧执行路径前统一校验；未知 variant/metric/engine/scalar 和非法 axis 组合返回 `422`，不再静默采用默认 Mandelbrot/OpenMP/fp64。
- [x] 不存在的 status/manifest/cancel run 统一返回结构化 `404 COMPUTE_RUN_NOT_FOUND`。
- [x] Compute v1 的 ln-map 与 HS mesh 改为真正后台 run；创建立即返回 queued，支持轮询/协作取消，并记录 kernel 硬件执行证据；旧 `/api` 默认同步行为保持不变。
- [x] Compute v1 HS field 改为后台 run，输出 float64 二进制 field 与 JSON sidecar manifest artifact，支持取消和硬件证据；旧 `/api/hs/field` 继续返回内联 base64。
- [x] Compute v1 transition mesh 改为后台 run；后台任务持有 transition/CPU/CUDA 资源租约，支持 volume 协作取消并报告实际 volume engine/scalar。
- [x] Compute v1 transition voxels 改为后台 run；保留旧接口内联几何，Compute manifest 统一保存 STL、硬件证据和取消终态。
- [x] Special points enumerate 改为 Compute v1 后台 run，search 保持既有后台调度；两者统一补齐 OpenMP/fp64 kernel 完成证据、JSON artifact 和取消终态。
- [x] Benchmark 改为后台 run/取消；manifest 使用 `multi_path` 证据逐 candidate 保存 requested/actual engine/scalar、样本数、吞吐与回退原因，不虚构单一 engine。
- [x] zoom/transition video 在 kernel 完成点报告 `kernelReported` 与实际 engine/scalar；真实 HTTP 合同验证 MP4 和硬件类别。
- [x] `legacy_zoom_video` 在 Compute v1 下真正后台执行，创建立即返回 queued，保留旧 `/api/video/zoom` 默认同步行为，并支持协作取消。
- [x] 将 Compute v1 文档提升为服务后端可独立实现的规范：公共 transport/状态/错误合同、18 个 kind 的 payload/默认值/限制/产物参考，以及 FastAPI ComputeClient、Outbox、轮询取消、artifact 摄取和硬件验收指南。
- [x] 文档覆盖检查从 C++ 单一能力注册表提取 kind，要求 18 个任务章节一一对应，并检查全部私有端点与 Worker 安全不变量，避免新增能力后文档静默漏项。
- [x] 补齐从零调用手册：明确服务 Key 由部署方生成并双端注入，不存在用户申请端点；解释 job kind 与 benchmark workload 的区别，并给出 preview/run/poll/cancel/download、参数化 DSL、M/B sequence、transition rotation/zoom/mesh、HS、special points 和 benchmark 的可复制请求。
- [x] 修正 DSL 文档中 `parameters` 被误写成数组的问题；规范为 object，并用真实 HTTP 验证实数及 `{re,im}` 复数参数。
- [x] 补齐 UI/导出细节合同：progress 字段 optional、stage-local 百分比与主要 stage；二维 `rotationDeg` 的精确坐标公式及其与 transition theta 的区别；RGBA preview 与异步 `map.png` 商业导出的完整分流和校验流程。

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
- [x] Orbit Program 同时传播到 zoom preview/export 的最终笛卡尔帧与 ln-map 条带；真实 HTTP export 生成 MP4 manifest。
- [x] 外部 ln-map 复用校验 Orbit hash；统一 zoom 拒绝 hash 不一致，legacy zoom 从已校验 sidecar 恢复 Orbit，并将其写入不可变 run 快照。
- [x] axis transition/transition video 保持独立顶层数学，普通 Orbit Program 请求显式 `422`，不转换为线性 blend。

### Compute Product Roadmap / 后续计算玩法

以下能力在原计划中即属于底座之后的分批启用项，不计入 C1/C2 完成条件；在 capabilities 中保持 `false`，请求不会静默降级：

- [ ] `weighted_schedule`：确定性 smooth weighted round-robin，只调度离散公式。
- [ ] `output_blend`：独立二维动力系统；权重归一化后组合复数输出，默认无逃逸证书。
- [ ] 权重/参数随迭代或视频时间变化的曲线与动画。
- [ ] 已审核热门 AST 的 SIMD/CUDA kernel 生成；不得改变 DSL 语义或规范化 hash。
- [ ] 原生 `repeat_block` 仅在扁平展开无法满足规模/调试需求时进入 Compute 新 IR；首期由 Platform 编辑 IR 编译为现有 sequence。

### Orbit 配方产品任务

- [x] 定义 repeat block 的有限重复、不重置 `z/c/n` 语义，以及展开到扁平 Compute v1 sequence 的兼容策略。
- [x] 划分 Compute 执行与 Platform Recipe/Draft/Revision/SavedView 存档边界。
- [x] 发布服务后端和前端实施、限制、迁移与验收清单，见 [Orbit 编排与配方存档任务](orbit_recipe_product_tasks.md)。
- [ ] Platform 实现编辑 IR、权威编译器、不可变版本、草稿并发控制和 SavedView API。
- [ ] 前端实现时间线、repeat block 分组、DSL picker、保存/版本/视角恢复及响应式交互。

### P0 — FastAPI 架构底座

- [x] Python 3.12 项目、配置、日志、request ID 和健康检查。
- [x] SQLAlchemy async 模型与 Alembic 初始迁移。
- [x] `render_jobs`、`quota_reservations`、`outbox_events`。
- [x] 强类型 ComputeClient 和结构化错误映射。
- [x] `SKIP LOCKED` 租约、至少一次投递和指数退避。
- [x] submit/poll/cancel/manifest 校验代码闭环；Worker 对每个 artifact 做鉴权流式读取并严格核对字节数/SHA-256，校验失败不进入 completed。
- [x] development/test 固定主体；生产配置拒绝启用未认证 Studio 路由。
- [x] FastAPI 到真实 C++ Compute 的 64×64 RGBA preview 冒烟。
- [ ] 在真实 PostgreSQL 上执行迁移并验证 Outbox render/cancel E2E。
- [ ] 接入 Redis 原子配额；当前底座只有 PostgreSQL reservation。

### D0/F0 — 部署与前端迁移

- [x] 本地 Compose、迁移 one-shot 服务和健康依赖配置。
- [x] 前端 lockfile 可重复安装；移除未使用且与当前 Three peer 冲突的 `@google/model-viewer`，生产依赖 audit 为 0。
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
| 2026-07-23 | Compute v1 HTTP contract | `compute_v1_http_contract` covers auth, capabilities, RGBA preview, background run, manifest and artifact streaming | passed; full CTest 7/7 at initial implementation |
| 2026-07-23 | Compute idempotency | pytest contract repeats the same key and asserts the same run ID | passed; full CTest 7/7 at initial implementation |
| 2026-07-23 | Platform unit tests | isolated Python 3.12 venv; `pytest -q platform-backend/tests` | 5 passed |
| 2026-07-23 | Final Compute/backend audit | Release rebuild; full CTest; real-process Compute HTTP pytest; Platform unit suite; JSON fence and local-link audit | 9/9 CTest; 74/74 HTTP; 8/8 Platform; docs checks passed |
| 2026-07-23 | Silent fallback/idempotency regression | unknown advertised fields and axis variants must return structured 422; reused key with changed payload must return 409 | passed over real HTTP |
| 2026-07-23 | Resource/error regression | missing status, manifest and cancel targets return `404 COMPUTE_RUN_NOT_FOUND`; Platform accepts structured and legacy string errors | passed |
| 2026-07-23 | Artifact integrity ingestion | Platform ComputeClient streams artifact bytes and rejects size/SHA-256 mismatch before Worker completion | 5/5 focused client tests passed |
| 2026-07-23 | Orbit product handoff | repeat block semantics, flat v1 compiler boundary, recipe/revision/saved-view ownership and backend/frontend task lists | documented; implementation remains pending |
| 2026-07-23 | Platform migration | `alembic upgrade head --sql` using PostgreSQL dialect | passed; 73 lines generated |
| 2026-07-23 | Platform preview integration | Uvicorn -> Compute v1 -> OpenMP 64×64 RGBA; propagated engine/scalar/request headers | passed |
| 2026-07-23 | Compose validation | `docker compose -f docker-compose.dev.yml config -q` | passed; daemon start unavailable to current user |
| 2026-07-23 | Safe DSL/Orbit unit contract | `orbit_program_smoke` covers parser, constants/parameters, canonical hash, limits and M/B deterministic sequence | passed |
| 2026-07-23 | Strict escape regression | `compute_path_diff` covers certified escape, unverified run-to-limit and numerical-divergence classification | passed |
| 2026-07-23 | Orbit HTTP/run contract | `compute_v1_http_contract` covers capabilities, sequence preview, raw strict field, compile errors, async manifest certificate and native compiler denial | passed; full CTest 8/8 at initial implementation |
| 2026-07-23 | HS Orbit parity/strictness | `hs_orbit_smoke` covers builtin parity, M/B sequence determinism and unverified run-to-limit | passed |
| 2026-07-23 | HS Compute run | pytest HTTP contract creates an HS mesh with Orbit sequence and verifies GLB/STL manifest entries and radius certificate | passed; full CTest 9/9 |
| 2026-07-23 | Typed Orbit AST | `orbit_program_smoke` distinguishes real/complex parameters in canonical hashes and verifies function result/promotion types | passed; full CTest 9/9 |
| 2026-07-23 | Output-blend escape counterexample | 50% Mandelbrot/Burning Ship complex-output DSL blend must report `certifiedRadius=null`; overflow remains numerical divergence, never escape | passed in `orbit_program_smoke` and `compute_path_diff` |
| 2026-07-23 | Real Compute HTTP matrix | backend subprocess + Bearer HTTP covers 400/401/422 envelopes, binary/JSON previews, async poll/cancel, manifest, artifact and Range 206 | passed in `compute_v1_http_contract` |
| 2026-07-23 | Orbit ln-map HTTP parity | Compute run renders escape/hist_eq sequence strips; builtin Orbit PNG SHA-256 equals legacy builtin PNG | passed; full CTest 9/9 |
| 2026-07-23 | Orbit zoom HTTP E2E | real preview creates start/end/strip/final frames; async export is polled to completion and manifest contains MP4 plus escape certificate | passed; full CTest 9/9 |
| 2026-07-23 | HTTP test maintainability | `pytest -q backend/src/tests/compute_v1 ...`; 11 test modules, fixture-managed real backend, no cross-test run/artifact sharing | 24/24 passed in 5.12s |
| 2026-07-23 | Independent HTTP test | direct node selection of `test_validation.py::test_invalid_dsl_returns_unknown_function` | 1/1 passed in 0.12s |
| 2026-07-23 | Test size rule | AST audit of every `test_*` function | all <= 12 lines; limit is 40 |
| 2026-07-23 | Post-refactor CTest | `ctest --test-dir backend/build --output-on-failure` | 9/9 passed; `compute_v1_http_contract` invokes pytest |
| 2026-07-23 | Hardware capability telemetry | authenticated capabilities reports CPU core/OpenMP/AVX and CUDA compile/runtime/device/VRAM state | passed in pytest HTTP contract |
| 2026-07-23 | Verified hardware execution | OpenMP map completion must report `kernelReported=true`, actual engine/scalar and CPU class in run status and manifest | passed in pytest HTTP contract |
| 2026-07-23 | CUDA execution/fallback | request CUDA and branch on runtime capability: CUDA runtime requires actual GPU/hybrid evidence; unavailable CUDA requires explicit CPU fallback | passed against current advertised runtime state |
| 2026-07-23 | Hardware telemetry regression | `pytest -q backend/src/tests/compute_v1 ...` and full CTest | 28/28 pytest; 9/9 CTest |
| 2026-07-23 | Platform regression after manifest extension | from `platform-backend/`: `pytest -q tests` | 5/5 passed |
| 2026-07-23 | Async ln-map lifecycle | Compute create returns queued; background render reaches completed; cancel request reaches cancelled; manifest reports actual engine/scalar | passed over real HTTP |
| 2026-07-23 | Async HS mesh lifecycle | Compute create returns queued; background mesh reaches completed; cooperative Orbit cancellation reaches cancelled; GLB/STL manifest keeps parity | passed over real HTTP |
| 2026-07-23 | Async/hardware regression | domain pytest suite, full CTest, and Platform unit tests | 33/33 pytest; 9/9 CTest; 5/5 Platform |
| 2026-07-23 | Async HS field lifecycle | queued run produces float64 binary + JSON metadata, reports OpenMP kernel evidence, supports cancellation, and preserves legacy inline response | 4/4 focused HTTP tests passed |
| 2026-07-23 | Async transition mesh | queued run owns its resource lease through completion, emits GLB/STL, reports actual volume hardware, supports cancellation, and preserves legacy sync response | focused HTTP tests passed |
| 2026-07-23 | Async transition voxels | queued run owns its resource lease, produces STL manifest, reports actual volume hardware, supports cancellation, and preserves legacy inline geometry | focused HTTP tests passed |
| 2026-07-23 | Post-transition regression | full domain HTTP suite, CTest, Platform tests and test-size audit | 45/45 pytest; 9/9 CTest; 5/5 Platform; longest test 14 lines |
| 2026-07-23 | Special point run lifecycle | enumerate returns queued and supports cancel; enumerate/search manifests contain JSON report and OpenMP/fp64 completion evidence | focused HTTP tests passed |
| 2026-07-23 | Post-special-points regression | full domain HTTP suite, CTest and Platform unit tests | 49/49 pytest; 9/9 CTest; 5/5 Platform |
| 2026-07-23 | Async hardware benchmark | queued/cancel lifecycle and per-candidate requested/actual path evidence in manifest | focused HTTP tests passed |
| 2026-07-23 | Video hardware completion | zoom and transition MP4 runs report kernel completion telemetry from returned render stats, not requested values | 8/8 focused HTTP tests passed |
| 2026-07-23 | Orbit ln-map reuse | independent ln-map sidecar records generation identity; matching Orbit hash exports, mismatch fails, legacy zoom restores immutable Orbit snapshot | 5/5 zoom HTTP tests passed |
| 2026-07-23 | Legacy zoom async lifecycle | Compute create returns queued, Orbit sidecar export completes with verified hardware, and a large render reaches cancelled | 15/15 focused video/cancellation HTTP tests passed |
| 2026-07-23 | Capability registry | `jobs[]` drives advertised kind sets and request validation; contract checks per-job Orbit/metric/engine/scalar/output metadata | 9/9 focused capabilities/validation HTTP tests passed |
| 2026-07-23 | Compute foundation final regression | full real-process HTTP suite, full CTest, Platform unit tests and test-size audit | 60/60 pytest; 9/9 CTest; 5/5 Platform; longest test 14 lines |
| 2026-07-23 | Deployment contract | `docker compose -f docker-compose.dev.yml config -q`; secure Compute container defaults and reduced Docker context | passed; daemon execution remains unavailable to current user |
| 2026-07-23 | Frontend dependency install | remove unused model-viewer peer conflict; `npm ci`, `npm ls --all`, `npm audit --omit=dev`, production Vite build | passed; production dependencies have 0 vulnerabilities; Vite 5 dev-only major-upgrade advisories remain documented |
| 2026-07-23 | Service-backend documentation handoff | normative transport contract + 18-kind job reference + FastAPI/Outbox integration guide; registry-driven documentation audit | 3/3 documentation checks passed |
| 2026-07-23 | Post-documentation real HTTP regression | `pytest -q backend/src/tests/compute_v1 --backend-binary=backend/build/fractal_studio_backend --studio-root=.` | 63/63 passed in 21.75s |
| 2026-07-23 | Documentation handoff final regression | `ctest --test-dir backend/build --output-on-failure`; from `platform-backend/`, `python -m pytest -q tests` | 9/9 CTest (including real HTTP); 5/5 Platform passed |
| 2026-07-23 | Compute cookbook usability | Key/workload/DSL/sequence/transition cookbook; parse every complete JSON block; real HTTP parameterized DSL preview | 8/8 JSON examples valid; 4/4 documentation checks; focused HTTP passed |
| 2026-07-23 | Post-cookbook Compute regression | full Compute pytest and CTest | 65/65 pytest in 26.91s; 9/9 CTest in 25.31s |
| 2026-07-23 | Progress/rotation/PNG details | real HTTP rotated map export, PNG signature/name, terminal map progress, documentation coverage | focused 15/15; full Compute 67/67 in 22.76s; CTest 9/9 in 26.12s |

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
| `1464262` | Orbit Program 同步接入 zoom preview/export 的笛卡尔帧与条带，并以真实 HTTP 生成 MP4。 |
| `823c319` | 删除 400 行单函数 HTTP 脚本，迁移为 24 个可独立运行的 pytest 合同测试、后端 fixture、API client 和 payload factory。 |
| `9ece9de` | Compute capabilities、run status 与 manifest 增加可验证硬件执行 telemetry，并测试 OpenMP 实际执行及 CUDA/CPU 回退真实性。 |
| `01683cc` | Compute v1 的 ln-map 与 HS mesh 改为真正异步 run，加入协作取消、后台生命周期保护及 kernel 完成 telemetry。 |
| `7b6eb1e` | HS field 接入异步 run/取消/硬件证据，并将 float64 网格与元数据纳入可校验 manifest artifact。 |
| `db398ec` | Transition mesh 接入后台 run，延长资源租约至任务结束，并为 volume kernel 增加协作取消与硬件证据。 |
| `d955d8d` | Transition voxels 接入后台 run/取消/硬件证据，后台持有资源租约，旧接口继续提供内联几何。 |
| `73c4f95` | Special points enumerate/search 统一后台 run telemetry、JSON artifact、取消状态与 OpenMP/fp64 硬件证据。 |
| `b668599` | Benchmark 接入异步 run/取消，并以 multi-path manifest 保存各硬件候选的实际执行和回退证据。 |
| `ac54b88` | Zoom/transition 视频从实际 kernel 返回值记录完成硬件证据，并新增 MP4 HTTP 合同测试。 |
| `1beccbf` | 外部 ln-map 加入 Orbit hash/生成身份一致性校验，legacy zoom 恢复 Orbit 不可变快照。 |
| `50cc5a9` | 发布 Compute 构建运维说明、私有 HTTP 合同、测试入口和商业安全边界。 |
| `4499d3c` | Compute 容器默认关闭 legacy/原生编译、使用非 root 用户，并缩减 Docker 构建上下文。 |
| `5c3d54e` | `legacy_zoom_video` 接入真实后台 run/queued 响应与协作取消，旧 API 默认同步语义保持不变。 |
| `f3a5fa2` | Compute job 单一注册表统一生成 kind 清单、入口校验与逐任务兼容矩阵。 |
| `5a6ad7b` | 移除未使用且与 Three 冲突的 model-viewer，更新 lockfile/PostCSS 并恢复可重复前端安装。 |
| `af961ba` | 发布服务后端可独立实施的 Compute transport 合同、18-kind 任务参考和 Platform Worker 对接指南。 |
| `89fa4ce` | 增加能力注册表驱动的文档覆盖测试，并从项目/Compute 文档入口链接新合同。 |
| `d63f893` | 增加从 Key 配置到各玩法真实请求的 Compute v1 调用手册，并修正 DSL parameters 示例。 |
| `7d4da38` | 自动检查调用手册关键内容，并通过真实 HTTP 验证参数化 DSL 请求。 |
| `1a4e485` | 明确各类任务 progress stage、二维 rotationDeg 坐标语义和异步 PNG 导出/下载流程。 |
| `334adca` | 真实 HTTP 验证旋转 map.png 导出、PNG 下载和 terminal progress 字段。 |
| `b8d85aa` | 能力字段统一拒绝静默降级；幂等键绑定请求 hash，并修正文档 JSON 示例。 |
| `aa54144` | status/manifest/cancel 对不存在 run 返回结构化 Compute 404，并增加独立 HTTP 回归。 |
| `3bc8ed4` | Platform Worker 流式读取 Compute artifact，严格验证大小/SHA-256；增强客户端错误映射。 |

## Delivery Rules / 交付规则

1. 每次状态变化都更新本文的里程碑、当前工作和验证记录。
2. 能力支持必须来自代码注册表；文档不能单独宣称 kernel 未实现的能力。
3. 不支持的组合显式失败，禁止静默替换 variant、Orbit Program 或数学语义。
4. 现有本地 SQLite 历史只读保留，不迁入商业 PostgreSQL。
5. `platform-backend-spec.zh.pdf` 是 M1–M7 领域与公共 API 的权威基线。
6. HTTP 合同测试按可命名行为拆分；单个测试不超过 40 行，禁止依赖前序测试产生的 run 或 artifact。
7. 硬件验收以 kernel 完成点报告的实际 engine/scalar 为准；请求参数本身不构成 GPU/CPU 已执行的证据，任何回退必须显式记录。

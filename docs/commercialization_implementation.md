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
| C2 | 安全 DSL 与 Orbit Program | pending | DSL 无原生代码执行；单公式和周期序列接入通用 CPU 渲染；严格逃逸元数据完成。 |
| P0 | FastAPI 架构底座 | pending | Python 工程、迁移、ComputeClient、Outbox Worker 和真实渲染闭环完成。 |
| D0 | 本地部署底座 | pending | Compose 启动 API、Worker、PostgreSQL、Redis、MinIO、Compute 并通过健康检查。 |
| F0 | 前端双轨接入 | pending | 保持现有设计；Platform/legacy API 可按环境切换；现有页面无回归。 |
| M1 | 身份与工作室 | pending | 不透明会话、RBAC、不可变配方、配额、预览和渲染任务完成。 |
| M3 | 资产与媒体库 | pending | Compute manifest 校验、对象存储、衍生文件和受保护下载完成。 |
| M4 | 市场与许可证 | pending | 草稿、发布快照、目录、许可证完成。 |
| M5 | 支付与权益 | pending | 支付宝通知、不可变订单、权益和下载闭环完成。 |
| M6 | 账本与结算 | pending | 只追加账本和人工打款流程完成；API 打款保持关闭。 |

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
- [ ] 将仍同步执行的 mesh/ln-map legacy job 改为统一后台 run。

### C2 — 安全 DSL 与 Orbit Program

- [ ] Pratt parser、typed AST、规范化和稳定 hash。
- [ ] 资源上限、错误位置和字节码解释器。
- [ ] `z`、`c`、`n`、参数、`i/pi/e` 和现有函数兼容。
- [ ] Formula/Sequence IR 与 Mandelbrot/Burning Ship 周期序列。
- [ ] `certified_finite`、`no_finite_bound`、`unverified` 分析结果。
- [ ] 未认证公式严格跑满最大迭代，数值溢出不标记为数学逃逸。
- [ ] 生产禁用 legacy 动态编译。

### P0 — FastAPI 架构底座

- [ ] Python 3.12 项目、配置、日志、request ID 和健康检查。
- [ ] SQLAlchemy async 模型与 Alembic 初始迁移。
- [ ] `render_jobs`、`quota_reservations`、`outbox_events`。
- [ ] 强类型 ComputeClient 和错误映射。
- [ ] `SKIP LOCKED` 租约、至少一次投递和指数退避。
- [ ] submit/poll/cancel/manifest 校验闭环。
- [ ] development/test 固定主体；生产禁用未认证 Studio 路由。

### D0/F0 — 部署与前端迁移

- [ ] 本地 Compose 和服务健康依赖。
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

## Delivery Rules / 交付规则

1. 每次状态变化都更新本文的里程碑、当前工作和验证记录。
2. 能力支持必须来自代码注册表；文档不能单独宣称 kernel 未实现的能力。
3. 不支持的组合显式失败，禁止静默替换 variant、Orbit Program 或数学语义。
4. 现有本地 SQLite 历史只读保留，不迁入商业 PostgreSQL。
5. `platform-backend-spec.zh.pdf` 是 M1–M7 领域与公共 API 的权威基线。

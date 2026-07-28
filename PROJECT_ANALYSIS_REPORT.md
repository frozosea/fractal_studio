# Fractal Studio — 全栈项目综合分析报告

> 分析日期：2026-07-28 | 基线：`master` | 方法：全源码+配置+文档+迁移脚本审计

---

## 一、项目整体架构

### 1.1 三层服务拓扑

```
┌─────────────────────────────────────────────────────────────────────┐
│  Browser (localhost:3010)                                           │
│  Next.js 14 App Router — React 18 + Tailwind CSS + Radix UI        │
│  /platform/v1/*  ⟶  Next rewrites proxy to Platform API            │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ HTTP (fs_session cookie + CSRF)
┌──────────────────────────────▼──────────────────────────────────────┐
│  Platform API (localhost:18100)                                     │
│  Python FastAPI — uvicorn — SQLAlchemy 2.0 async — asyncpg          │
│  认证 / 食谱 / 渲染编排 / 资产 / 市场 / 支付 / 分账                  │
│  POST /compute/v1/*  ⟶  Bearer Token 私有通道                       │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ Bearer Token (FSD_COMPUTE_SERVICE_KEY)
┌──────────────────────────────▼──────────────────────────────────────┐
│  Private Compute (localhost:18101)                                  │
│  C++20 原生 HTTP 服务 — SQLite — OpenCV — OpenMP/AVX2/AVX-512/CUDA │
│  19 种计算 kind / 分形数学 / 产物管理 / 硬件证据                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.2 目录分工

| 目录 | 语言/运行时 | 角色 |
|---|---|---|
| `frontend/` | TypeScript / Next.js 14 (App Router) | 浏览器 UI |
| `platform-backend/` | Python 3.12+ / FastAPI + uvicorn | 业务编排、认证、资产、市场、支付 |
| `backend/` | C++20 / 原生 HTTP（自实现） | 私密分形计算引擎 |
| `docs/` | Markdown × 22 篇 | 架构、合同、开发、测试文档 |
| `scripts/` | Bash | 系统检查、启动脚本、旧版完整性校验 |
| `platform-backend/migrations/` | Alembic + SQL | PostgreSQL 迁移（14 个版本） |

### 1.3 基础设施

| 组件 | 镜像 | 端口 | 用途 |
|---|---|---|---|
| PostgreSQL 16 | `postgres:16-alpine` | 15442 | 用户、资产、订单、账本 |
| Redis 7 | `redis:7-alpine` | 16389 | 会话缓存、限流、配额 |
| MinIO | `minio/minio:RELEASE.2025-04-22` | 19010 (API) / 19011 (Console) | S3 兼容对象存储 |
| Alipay Stub | 自构建 | 18102 | 本地支付宝模拟 |
| C++ Compute | 自构建 (Debian bookworm) | 18101 | 分形计算引擎 |

---

## 二、技术栈拆解

### 2.1 前端（`frontend/`）

| 层面 | 选型 | 版本 |
|---|---|---|
| 框架 | **Next.js 14** (App Router, React Server Components) | `^14.2.0` |
| UI 库 | **React 18** | `^18.3.0` |
| 样式 | **Tailwind CSS 3** + `tailwindcss-animate` + CSS 变量主题 | `^3.4.0` |
| 组件 | **Radix UI** (Dialog, DropdownMenu, Select, Slider, Tabs, Toast, Tooltip, Avatar, Slot) | `^1.1.x–^2.1.x` |
| 图标 | **Lucide React** | `^0.468.0` |
| 表单 | **React Hook Form** + **Zod** + `@hookform/resolvers` | `^7.54.0` / `^3.24.0` |
| 状态管理 | **Zustand** (3 stores: auth, studio, ui) | `^5.0.0` |
| 数据请求 | **TanStack React Query** (前身为 React Query) | `^5.62.0` |
| 国际化 | **next-intl** | `^3.26.5` |
| 工具函数 | `clsx` + `tailwind-merge` + `class-variance-authority` | — |
| 端到端测试 | **Playwright** | `1.60.0` |
| 包管理 | **pnpm** | lockfile v9 |

**页面路由结构（App Router）：**

```
src/app/
├── [locale]/                         ← next-intl 语言路由 (en/zh)
│   ├── layout.tsx                    ← 根布局
│   ├── page.tsx                      ← 首页
│   ├── (auth)/                       ← 认证路由组
│   │   ├── layout.tsx
│   │   ├── login/page.tsx
│   │   └── register/page.tsx
│   └── (workbench)/                  ← 工作台路由组
│       ├── layout.tsx
│       ├── studio/page.tsx           ← 核心：分形工作室
│       ├── assets/page.tsx           ← 我的资产
│       ├── explore/page.tsx          ← 探索市场
│       ├── listings/page.tsx         ← 我的挂牌
│       ├── purchases/page.tsx        ← 已购
│       ├── favorites/page.tsx        ← 收藏
│       ├── finance/page.tsx          ← 创作者余额
│       └── payouts/page.tsx          ← 提现
├── globals.css
└── layout.tsx                        ← 根布局（无语言前缀）
```

### 2.2 平台后端（`platform-backend/`）

| 层面 | 选型 | 版本 |
|---|---|---|
| Web 框架 | **FastAPI** (含 uvicorn) | `>=0.115` |
| ORM | **SQLAlchemy 2.0** (async) + **asyncpg** | `>=2.0` |
| 迁移 | **Alembic** | `>=1.16` |
| 序列化 | **orjson** | `>=3.10` |
| 密码哈希 | **hashlib.scrypt** (Python stdlib, n=2^14) | — |
| 会话 | 不透明 `fs_session` HttpOnly cookie | — |
| CSRF | HMAC-SHA256(session_secret, session_token) | — |
| 幂等 | 自实现 Idempotency Service (DB lease + scope/key) | — |
| 对象存储 | **boto3** → S3/MinIO | `>=1.35` |
| 支付 | 自实现支付宝网关适配器 + Stub 模式 | — |
| HTTP 客户端 | **httpx** (async) | `>=0.28` |
| 后台任务 | **Outbox Pattern** (自实现 Worker + HandlerRegistry) | — |
| 重试 | **tenacity** | `>=9` |
| 图片处理 | **Pillow** | `>=11` |
| QR 码 | **qrcode** + **zxing-cpp** (解码) | `>=2.3` |
| 结构化日志 | **structlog** | `>=24.4` |
| 测试 | **pytest** + **pytest-asyncio** | `>=8.3` |
| Lint | **ruff** | `>=0.9` |
| 包管理 | **uv** (Astral) | `0.11.31` |

**模块分层（Clean-ish Architecture）：**

```
app/
├── main.py                    ← FastAPI app + 中间件 + 全局异常处理
├── core/                      ← 配置 / DB / 访问中间件 / 幂等 / 审计 / 日志
├── auth/                      ← 注册 / 登录 / 会话 / 创作者资料 / 限流
├── studio/                    ← 配方 / 渲染任务 / 预览 / 能力查询 / 映射器
├── assets/                    ← 资产库 / 文件 / 预览 / 媒体转码 / 清理
├── marketplace/               ← 挂牌 / 探索 / 收藏 / 许可协议
├── commerce/                  ← 下单 / 支付通知 / 支付宝 Webhook
├── finance/                   ← 创作者余额 / 提现请求 / 人工打款 / 账本
├── outbox/                    ← 事务外发事件 / Worker / 调度器
└── infrastructure/            ← 外部适配器
    ├── compute/               ← ComputeClient (HTTP → C++ Compute)
    ├── storage/               ← S3/MinIO 对象存储
    ├── redis/                 ← 配额 / 限流
    └── alipay/                ← 支付宝支付网关
```

### 2.3 计算后端（`backend/`）

| 层面 | 选型 |
|---|---|
| 语言 | **C++20** |
| 构建 | **CMake 3.18+** |
| HTTP | **自实现** (POSIX socket, 每连接 detached thread) |
| 数据库 | **SQLite 3** (本地 run 状态) |
| JSON | **nlohmann/json** (header-only) |
| 图像 | **OpenCV** (core, imgcodecs, imgproc, videoio) |
| 加密 | **OpenSSL** (SHA-256, 常量时间比较) |
| 并行 | **OpenMP** / **AVX2** / **AVX-512** / **CUDA** (可选) |
| 高精度 | `__float128` (libquadmath) / **MPFR** (任意精度) |
| 视频 | **FFmpeg** (运行时依赖) |
| 容器 | Debian bookworm-slim |

**计算能力：19 种 kind，当前 Platform 接入 5 种：**

| Kind | 持久化 | 当前状态 |
|---|---|---|
| `map_image` | ✓ | **已接入** |
| `transition_image` | ✓ | **已接入** |
| `zoom_video` | ✓ | **已接入** (depthOctaves 硬编码) |
| `hs_mesh` | ✓ | **已接入** (仅取一个格式) |
| `transition_mesh` | ✓ | **已接入** (仅取一个格式) |
| `raw_field` / `ln_map` / `video_preview` / `legacy_zoom_video` / `transition_video` / `transition_video_preview` / `hs_field` / `transition_voxels` / `special_points_*` × 5 / `benchmark` | — | **未接入**（14 个） |

---

## 三、前后端交互逻辑

### 3.1 请求链路

```
Browser (Next.js)
  │
  ├─ /platform/v1/*  ──Next rewrites──▶  FastAPI :8000/v1/*
  │
  └─ middleware.ts: next-intl 语言路由
     (排除 /platform, /api, /_next 等路径)

Platform API (FastAPI)
  │
  ├─ 鉴权: fs_session cookie → session_service.resolve() → AccessPrincipal
  ├─ 写操作: Origin 检查 + CSRF Token (HMAC-SHA256)
  ├─ 幂等: Idempotency-Key header + DB lease
  ├─ RBAC: require_role("creator") / require_role("finance_operator")
  │
  └─ ComputeClient  ──Bearer Token──▶  C++ Compute /compute/v1/*
```

### 3.2 鉴权机制

**Platform API（FastAPI）：**
- **会话**：不透明 `fs_session` HttpOnly cookie
- **生成**：`secrets.token_urlsafe(32)` → SHA-256 hash 存入 `sessions.token_hash`
- **过期**：`SESSION_TTL_DAYS` (默认 30 天)
- **CSRF**：`HMAC-SHA256(session_secret, raw_session_token)` → 前端每次获取 `/v1/auth/csrf-token`，写操作携带 `X-CSRF-Token`
- **Origin 检查**：`enforce_origin_and_csrf` — 同源请求免 CSRF；受信跨域（`CORS_ORIGINS`）需 CSRF
- **密码**：stdlib scrypt (n=2^14, r=8, p=1)，格式 `scrypt$N$r$p$salt_b64$hash_b64`
- **限流**：Redis — login 10/min, register 5/min
- **幂等**：`Idempotency-Key` header → DB 租赁（lease_seconds=30, TTL=24h）

**C++ Compute API：**
- **服务间鉴权**：`Authorization: Bearer <FSD_COMPUTE_SERVICE_KEY>`
- **常量时间比较**：防止时序攻击
- **例外**：`GET /compute/v1/health` 不鉴权

### 3.3 完整 API 清单

#### Public Platform Routes（`/v1/*`）

**认证 (auth)：**
| 方法 | 路径 | 鉴权 | 幂等 | CSRF | 说明 |
|---|---|---|---|---|---|
| POST | `/v1/auth/register` | 无 | — | Origin | 注册 → fs_session cookie |
| POST | `/v1/auth/login` | 无 | — | Origin | 登录 → fs_session cookie |
| POST | `/v1/auth/logout` | session | — | ✓ | 登出 → 撤销会话 |
| GET | `/v1/me` | session | — | — | 当前用户 + 角色 + 创作者资料 |
| GET | `/v1/auth/csrf-token` | session | — | — | 获取 CSRF 令牌 |
| PATCH | `/v1/me/creator-profile` | session | ✓ | ✓ | 设置创作者 handle/displayName |

**Studio (M2)：**
| 方法 | 路径 | 鉴权 | 幂等 | CSRF | 说明 |
|---|---|---|---|---|---|
| GET | `/v1/studio/capabilities` | session | — | — | Compute 运行时能力投影 |
| POST | `/v1/recipes` | session | ✓ | ✓ | 创建/复用不可变配方 → 201 |
| GET | `/v1/me/recipes` | session | — | — | 配方列表 (cursor 分页) |
| POST | `/v1/studio/preview` | session | — | ✓ | 实时预览 → image/png |
| POST | `/v1/render-jobs` | session | ✓ | ✓ | 创建持久渲染任务 |
| GET | `/v1/render-jobs/{id}` | session | — | — | 任务状态查询 |
| POST | `/v1/render-jobs/{id}/cancel` | session | ✓ | ✓ | 取消任务 |

**资产 (M3)：**
| 方法 | 路径 | 鉴权 | 幂等 | CSRF | 说明 |
|---|---|---|---|---|---|
| GET | `/v1/me/assets` | session | — | — | 我的资产 (cursor 分页+过滤) |
| GET | `/v1/me/assets/{id}` | session | — | — | 资产详情+预览URL |
| PATCH | `/v1/me/assets/{id}` | session | ✓ | ✓ | 修改可见性 |
| DELETE | `/v1/me/assets/{id}` | session | ✓ | ✓ | 软删除 → 204 |
| POST | `/v1/assets/{id}/download-url` | session | ✓ | ✓ | 生成预签名下载 URL |

**市场 (M4)：**
| 方法 | 路径 | 鉴权 | 幂等 | CSRF | 说明 |
|---|---|---|---|---|---|
| GET | `/v1/explore` | 可选 | — | — | 公开探索 (cursor+过滤+排序) |
| GET | `/v1/listings/{id}` | 可选 | — | — | 挂牌详情 |
| GET | `/v1/me/listings` | creator | — | — | 我的挂牌 |
| POST | `/v1/listings` | creator | ✓ | ✓ | 创建草稿 → 201 |
| PATCH | `/v1/listings/{id}` | creator | ✓ | ✓ | 更新草稿 |
| POST | `/v1/listings/{id}/publish` | creator | ✓ | ✓ | 发布 |
| POST | `/v1/listings/{id}/unpublish` | creator | ✓ | ✓ | 下架 |
| GET | `/v1/me/favorites` | session | — | — | 我的收藏 |
| POST | `/v1/assets/{id}/favorite` | session | ✓ | ✓ | 添加收藏 → 201 |
| DELETE | `/v1/assets/{id}/favorite` | session | ✓ | ✓ | 取消收藏 → 204 |

**商业 (M5)：**
| 方法 | 路径 | 鉴权 | 幂等 | CSRF | 说明 |
|---|---|---|---|---|---|
| POST | `/v1/checkout` | session | ✓ | ✓ | 下单 → 订单+支付宝表单 → 201 |
| GET | `/v1/orders/{id}` | session | — | — | 订单详情 |
| GET | `/v1/me/purchases` | session | — | — | 我的购买 (cursor 分页) |
| POST | `/v1/webhooks/alipay` | 无 | — | — | 支付宝异步通知 (不公开文档) |

**财务 — 创作者侧 (M6b)：**
| 方法 | 路径 | 鉴权 | 幂等 | CSRF | 说明 |
|---|---|---|---|---|---|
| GET | `/v1/me/payout-requests` | creator | — | — | 提现记录 |
| GET | `/v1/me/payout-requests/balance` | creator | — | — | 创作者余额 |
| POST | `/v1/me/payout-requests` | creator | ✓ | ✓ | 创建提现 (multipart: amount+qrCode) |
| POST | `/v1/me/payout-requests/{id}/cancel` | creator | ✓ | ✓ | 取消提现 |

**财务 — 运营侧 (M6b internal)：**
| 方法 | 路径 | 鉴权 | 幂等 | CSRF | 说明 |
|---|---|---|---|---|---|
| GET | `/internal/v1/payout-requests` | finance_operator | — | — | 运营：提现列表 |
| POST | `/internal/v1/payout-requests/{id}/mark-paid` | finance_operator | ✓ | ✓ | 运营：标记已打款 |
| POST | `/internal/v1/payout-requests/{id}/reject` | finance_operator | ✓ | ✓ | 运营：驳回 |

#### 计算后端路由（`/compute/v1/*`，仅 Platform Worker 访问）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/compute/v1/health` | 健康检查 (无需鉴权) |
| GET | `/compute/v1/capabilities` | 运行时能力 |
| POST | `/compute/v1/runs` | 创建异步计算 run (含幂等 Key) |
| GET | `/compute/v1/runs/{id}` | 查询 run 状态+进度 |
| GET | `/compute/v1/runs/{id}/manifest` | 获取完成 manifest+产物清单 |
| POST | `/compute/v1/runs/{id}/cancel` | 取消 run |
| GET | `/compute/v1/artifacts?artifactId=` | 流式下载产物原始字节 |

**响应结构规范：**
```json
// Platform: 统一包装
{ "data": {...}, "page": { "nextCursor": "..." | null } }  // 集合
{ "data": {...} }                                            // 单资源
// 错误
{ "error": { "code": "error_code", "message": "human readable", "details": {} } }

// Compute: 直接返回或
{ "data": { "computeRunId": "...", "status": "...", ... } }
// 错误
{ "error": { "code": "COMPUTE_ERROR_CODE", "message": "...", "details": {...} } }
```

### 3.4 异步回调处理

```
                    ┌──────────────────────┐
                    │   Outbox Worker      │
                    │   (独立进程)          │
                    └──────────┬───────────┘
                               │ poll (outbox_poll_interval_seconds)
                               ▼
         ┌─────────────────────────────────────────┐
         │  outbox_events 表                        │
         │  status: pending → leased → done/dead    │
         │  含 attempt_count + 指数退避              │
         └──────────┬──────────────────────────────┘
                    │
     ┌──────────────┼──────────────┬──────────────┐
     ▼              ▼              ▼              ▼
render.created  render.poll   render.cancel  payment.reconcile
     │              │              │              │
     ▼              ▼              ▼              ▼
submit_to_compute poll_status  forward_cancel  reconcile_alipay
     │              │              │              │
     ▼              ▼              ▼              ▼
   outbox:       outbox:       outbox:        order fulfilled/
 render.poll    reschedule    -               entitlement granted
 (chained)      or ingest
```

**关键事件类型：**
- `render.created.v1` — 提交到 Compute
- `render.poll.v1` — 轮询 Compute 状态（链式重调度）
- `render.cancel_requested.v1` — 转发取消到 Compute
- `render.quota_expired.v1` — 配额过期处理
- `media.create_derivatives.v1` — 生成缩略图/水印
- `payment.reconcile.v1` — 支付宝支付核对
- `cleanup.expired.v1` / `asset.cleanup_orphan.v1` — 清理
- `payout.qr_cleanup.v1` — 提现 QR 码清理

**调度器（DueWorkReader，定期触发）：**
- `RenderQuotaExpiryScheduler` — 过期配额→terminal
- `AssetCleanupScheduler` — 清理到期资产
- `IdempotencyExpiryScheduler` — 清理到期幂等记录
- `CommerceService` — 支付对账调度

---

## 四、数据库表结构

### 4.1 核心表（27 张）及 ER 关系

```
users ────────────────────────────────────────────────────────────────┐
  │ id (PK UUID)                                                      │
  │ email (UNIQUE)                                                    │
  │ password_hash (scrypt)                                            │
  │ status (active|disabled)                                          │
  │ created_at                                                        │
  │                                                                   │
  ├── user_roles (user_id PK, role PK)                                │
  │     role: creator | finance_operator                              │
  │                                                                   │
  ├── creator_profiles (user_id PK FK)                                │
  │     handle (UNIQUE, lowercase), display_name                      │
  │                                                                   │
  ├── sessions (id PK, user_id FK)                                    │
  │     token_hash (UNIQUE), expires_at, revoked_at                   │
  │     created_ip_hash, user_agent_hash                              │
  │     rotated_from_session_id (自引用 FK)                            │
  │                                                                   │
  ├── fractal_recipes (id PK, owner_id FK)                            │
  │     canonical_spec (JSONB), spec_hash (UNIQUE per owner)          │
  │     structure_version, renderer_version                           │
  │                                                                   │
  ├── render_jobs (id PK, owner_id FK, recipe_id FK)                  │
  │     status (9 状态状态机), idempotency_key (UNIQUE per owner)      │
  │     compute_run_id, output_kind, output_spec_json                 │
  │     progress_percent [0-100], error_code                          │
  │     compute_result_json, selected_artifact_ids_json               │
  │                                                                   │
  ├── quota_reservations (id PK, user_id FK, render_job_id FK UNIQUE) │
  │     quota_kind, units, status (reserved|released|expired)         │
  │                                                                   │
  ├── assets (id PK, owner_id FK, recipe_id FK, render_job_id FK UK)  │
  │     media_type, status, visibility, created_at                    │
  │     │                                                             │
  │     └── asset_files (id PK, asset_id FK)                          │
  │           purpose (master|thumbnail|watermarked_preview|...)       │
  │           object_key (UNIQUE), sha256, size_bytes, media_type     │
  │                                                                   │
  ├── favorites (user_id PK FK, asset_id PK FK)                       │
  │                                                                   │
  ├── listings (id PK, asset_id FK (部分唯一), creator_id FK)          │
  │     status (draft|published|unpublished|archived)                 │
  │     title, description, price_amount, currency (CNY)              │
  │     current_published_version_id FK → listing_versions            │
  │     │                                                             │
  │     ├── listing_versions (id PK, listing_id FK)                   │
  │     │     version, snapshot_json (不可变快照)                      │
  │     │                                                             │
  │     ├── licence_offers (id PK, listing_id FK)                     │
  │     │     code, terms_version, terms_json, is_active              │
  │     │                                                             │
  │     └── listing_tags (listing_id PK FK, tag PK)                   │
  │                                                                   │
  ├── orders (id PK, buyer_id FK)                                     │
  │     status (pending_payment|fulfilled|closed|payment_exception)   │
  │     amount, currency (CNY), paid_at                               │
  │     │                                                             │
  │     ├── order_items (id PK, order_id FK)                          │
  │     │     listing_id FK, listing_version_id FK                    │
  │     │     licence_offer_id FK, asset_id FK, creator_id FK         │
  │     │     price_amount, creator_amount, platform_fee_amount       │
  │     │     commission_policy_version, 快照 (listing+licence JSONB) │
  │     │     │                                                       │
  │     │     └── entitlements (id PK, user_id FK, asset_id FK,       │
  │     │           order_item_id FK UNIQUE)                          │
  │     │           status (active|revoked), granted_at               │
  │     │                                                            │
  │     ├── payment_attempts (id PK, order_id FK UNIQUE)              │
  │     │     out_trade_no (UNIQUE), alipay_trade_no (UNIQUE)         │
  │     │     status, amount, expires_at                              │
  │     │     │                                                       │
  │     │     └── payment_notifications (id PK, payment_attempt_id FK)│
  │     │           fingerprint (UNIQUE), trade_status, payload       │
  │     │                                                            │
  │     └── refund_reversals (id PK, order_id FK, payment_attempt_id FK UK)│
  │           amount, status, external_reference                      │
  │                                                                   │
  ├── creator_balances (creator_id PK FK)                             │
  │     available_amount, reserved_amount, currency (CNY)             │
  │                                                                   │
  ├── payout_requests (id PK, creator_id FK, operator_user_id FK)     │
  │     amount, currency (CNY), qr_object_key                        │
  │     status (pending|paid|rejected|cancelled)                      │
  │     external_reference, rejection_reason                          │
  │     (每个 creator 最多一个 pending ── 部分唯一索引)                 │
  │                                                                   │
  ├── ledger_entries (id PK, creator_id FK, order_item_id FK,         │
  │       payout_request_id FK)                                       │
  │     account (creator_available|creator_reserved|platform_revenue) │
  │     signed_amount, entry_type, created_at                         │
  │     (append-only, 每个 order_item 每种 entry_type 唯一)            │
  │                                                                   │
  ├── audit_events (id PK, actor_user_id FK, actor_type)              │
  │     action, subject_type, subject_id, metadata_json               │
  │                                                                   │
  ├── idempotency_records (id PK, user_id FK)                         │
  │     scope, idempotency_key (UNIQUE per user+scope)                │
  │     request_hash, status, response_json, lease_owner, expires_at  │
  │                                                                   │
  └── outbox_events (id PK)                                           │
        event_type, aggregate_type, aggregate_id, payload_json        │
        idempotency_key (UNIQUE per event_type+aggregate)             │
        status (pending|leased|done|dead), available_at               │
        lease_until, attempt_count, last_error_code                   │
```

### 4.2 关键约束和设计决策

- **金额**：统一 `NUMERIC(18,2)` + `CHECK currency = 'CNY'`
- **不可变快照**：`order_items` 包含 listing 和 licence 的 JSONB 快照，确保历史订单不受后续价格/条款变更影响
- **Append-only 账本**：`ledger_entries` 无 UPDATE 路径，`creator_balances` 由账本条目的代数和推导
- **幂等保护**：所有写操作使用 `Idempotency-Key` + `idempotency_records` DB 租赁
- **Outbox Pattern**：所有副作用通过 `outbox_events` 表外发，Worker 异步处理

### 4.3 状态机

**Render Job（9 状态）：**
```
queued → submitting → running → compute_succeeded → ingesting → completed
  │         │            │            │                 │
  └─────────┴────────────┴────────────┴─────────────────┘
                        │
                   cancel_requested → cancelled
                        │
                      failed
```

**Order：**
```
pending_payment → fulfilled / closed / payment_exception
```

**Payout Request：**
```
pending → paid / rejected / cancelled
```

---

## 五、现有缺陷与优化建议

### 5.1 高危 (High Severity)

| # | 问题 | 位置 | 影响 |
|---|---|---|---|
| H1 | **架构文档严重过时** | `docs/architecture.md` | 仍描述 Vue 3/Vite 前端、`/api/*` 路由、无 Platform 层——实际架构已完全重构为 Next.js → FastAPI → C++ 三层。任何新加入者会被误导。 |
| H2 | **进度文档与实际代码偏差巨大** | `docs/commercialization_implementation.md` | 标注「M1-M6 0%、前端双轨 0%、商业化约 30%」，实际全部落地并有 20 个 E2E 测试文件。已知问题 #12 已记录但未修复。 |
| H3 | **14 个 Compute kind 不可达** | `compute_request_mapper.py` | C++ 侧 19 个 kind 完整实现，Platform 只映射了 5 个。raw_field、ln_map、视频、特殊点、benchmark 对浏览器完全不可达。文档声称"本阶段聚焦二维"可以接受，但该数字本身就是 README 宣传与实际能力的偏差。 |
| H4 | **前端无 video/mesh 输出入口** | `studio/page.tsx` | 后端 ImageOutputSpec/VideoOutputSpec/HsMeshOutputSpec/TransitionMeshOutputSpec 四个判别联合已全部定义、mapper 和 worker 支持就绪，但前端只构造 `{ kind: "image", format: "png" }`。已知问题 #2。 |

### 5.2 中危 (Medium Severity)

| # | 问题 | 位置 | 影响 |
|---|---|---|---|
| M1 | **hs_mesh / transition_mesh 双产物丢弃一个** | `render_worker.py:_select_artifacts` | 按 `output_spec["format"]` 选 MIME 后 `[:1]`，glb/stl 只能二选一。Compute 合同规定二者必需。已知问题 #7。 |
| M2 | **session_secret 和 compute_service_key 默认值不安全** | `.env.example` / `docker-compose.dev.yml` | `dev-session-secret-change-me-32-bytes-minimum` 虽在 prod 验证中会拒绝，但本地开发未强制更换。 |
| M3 | **CORS 配置依赖环境变量字符串解析** | `config.py:trusted_origins` | CSV 格式的 `CORS_ORIGINS` 通过 `split(",")` 解析，对空格/空白敏感。虽有 `strip()`，但配置错误无启动时校验。 |
| M4 | **前端 `fallbackCapabilities` 硬编码** | studio page | Compute 不可用时用静态 fallback，engines 只列 `auto/openmp` 缺少 `cuda/avx2/avx512/hybrid`，用户选不支持的组合到提交才报 422。已知问题 #10。 |
| M5 | **`zoom_video` `depthOctaves` 硬编码 20.0** | `compute_request_mapper.py` | 虽然后端已修、VideoOutputSpec 支持 depthOctaves，但前端尚未暴露控件。用户无法控制视频变焦深度。已知问题 #3。 |
| M6 | **Alipay webhook 端点无签名验证文档** | `commerce/router.py` | `POST /v1/webhooks/alipay` 包含表单解析，但支付宝签名验证逻辑封闭在 `payment_gateway.py` 中，前端不可见、文档不公开。 |

### 5.3 低危 (Low Severity)

| # | 问题 | 位置 | 影响 |
|---|---|---|---|
| L1 | **旧版 API 路由仍在 CMakeLists 编译** | `backend/CMakeLists.txt` | `routes_runs.cpp`、`routes_modules.cpp`、`routes_map.cpp` 等旧 `/api/*` 路由仍在编译，即使 `FSD_ENABLE_LEGACY_API=0`。仅运行时禁用，二进制仍包含。 |
| L2 | **前端集合缓存 30 秒 TTL** | `platform.ts:collection()` | 内存在 `Map` 中，无失效机制（仅 POST/DELETE/PATCH 清全量），可能导致短暂数据不一致。 |
| L3 | **middleware.ts 只做语言路由** | `frontend/src/middleware.ts` | 只调用 `next-intl` 的 `createMiddleware`，不做鉴权检查。受保护页面是否可被未登录用户直接访问取决于 `auth-provider.tsx` 的客户端检查。 |
| L4 | **alembic.ini 含硬编码数据库 URL** | `alembic.ini` | `sqlalchemy.url = postgresql+psycopg://postgres:postgres@localhost:5432/fractal_platform`，Docker 通过环境变量 `MIGRATION_DATABASE_URL` 覆盖，但配置文件本身就包含凭据。 |
| L5 | **部分 Python 文件类型注解不完整** | 多处 | 例如 `request_context.py`、`audit_writer.py` 部分函数缺少返回类型注解。 |
| L6 | **C++ `main.cpp` 直接监听端口 18080** | `backend/src/main.cpp` | 无反向代理/TLS 终止层，生产部署需额外配置。虽 README 声明"生产应只监听私网"，但二进制本身无此限制。 |

### 5.4 代码质量观察

| 观察 | 详情 |
|---|---|
| **错误处理较规范** | Platform 有全局 HTTPException/validation/unexpected 三层异常处理器，结构化错误码（`code`/`message`/`details`），Compute 侧返回固定结构的 JSON 错误 |
| **幂等设计全面** | 所有写操作支持 `Idempotency-Key`，含 DB 租赁、冲突检测和重放响应 |
| **Outbox 模式成熟** | 独立 Worker 进程 + HandlerRegistry + 指数退避重试 + 死信队列 + 租约续期心跳 |
| **数据库约束充分** | Check constraints 覆盖金额非负、分账等于标价、状态一致性、唯一性等 |
| **RSockets 线程模型受限** | C++ HTTP server 每连接 detached thread，高并发下线程数膨胀 |
| **前端 API 层设计合理** | `platform.ts` 集中管理 42 个 API 函数，类型安全，CSRF/幂等自动处理，集合 30s 缓存 |

---

## 六、标准化总结

### 6.1 项目架构总览（文字版流程图）

```
┌─────────────────────────────────────────────────────────────────────┐
│                        FRACTAL STUDIO                               │
│                    三层微服务 + 异步 Worker                          │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────┐     /platform/v1/*      ┌──────────────────┐      │
│  │  Next.js 14  │ ──────────────────────▶ │  FastAPI Platform │      │
│  │  (React 18)  │ ◀── JSON { data: ... } │  (Python 3.12)   │      │
│  │  :3010       │                        │  :8000            │      │
│  └─────────────┘                        └──────┬───────────┘      │
│                                                │                    │
│  ┌──────────────────────────────┐              │                    │
│  │ Zustand stores               │     ┌────────▼──────────┐       │
│  │  auth / studio / ui          │     │ PostgreSQL 16      │       │
│  │                              │     │ (users, assets,    │       │
│  │ TanStack Query (cache)       │     │  orders, ledger)   │       │
│  │                              │     └───────────────────┘       │
│  │ next-intl (en/zh)            │                                  │
│  └──────────────────────────────┘     ┌──────────────────┐       │
│                                       │ Redis 7           │       │
│  ┌──────────────────────────────┐     │ (sessions, quota, │       │
│  │ Radix UI + Tailwind + CVA   │     │  rate-limit)      │       │
│  └──────────────────────────────┘     └──────────────────┘       │
│                                                                     │
│                              ┌──────────────────────┐              │
│                              │ Outbox Worker         │              │
│                              │ (独立进程, 异步消费)    │              │
│                              │ render / payment /    │              │
│                              │ cleanup / media       │              │
│                              └──────┬───────────────┘              │
│                                     │                               │
│  ┌──────────────────────────────────▼───────────────────────────┐  │
│  │  Private C++ Compute (:18080)                                │  │
│  │  ┌──────────┐ ┌──────────┐ ┌───────────┐ ┌──────────────┐   │  │
│  │  │ OpenMP   │ │ AVX2     │ │ AVX-512   │ │ CUDA (opt)   │   │  │
│  │  └──────────┘ └──────────┘ └───────────┘ └──────────────┘   │  │
│  │  ┌──────────┐ ┌──────────┐ ┌───────────┐ ┌──────────────┐   │  │
│  │  │ fp32/64  │ │ fx64     │ │ fp80/128  │ │ MPFR (arb)   │   │  │
│  │  └──────────┘ └──────────┘ └───────────┘ └──────────────┘   │  │
│  │  19 种 kind / SQLite / OpenCV / FFmpeg                       │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐  │
│  │ MinIO (S3)       │  │ Alipay Stub      │  │ Docker Compose   │  │
│  │ :19010           │  │ :18102           │  │ dev stand        │  │
│  └──────────────────┘  └──────────────────┘  └──────────────────┘  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 6.2 前后端分工与数据流

```
USER ACTION                          SYSTEM RESPONSE
──────────                          ────────────────

1. 用户注册/登录
   POST /v1/auth/register ──────▶  创建 users + sessions
   ◀────────── fs_session cookie    scrypt 哈希密码

2. 进入 Studio 调试分形
   GET /v1/studio/capabilities ──▶  ComputeClient → C++ /compute/v1/capabilities
   ◀────────── metrics/engines/     返回能力矩阵 → 前端控件渲染
                scalars/variants

3. 拖拽画布 → 实时预览
   POST /v1/studio/preview ──────▶  ComputeClient.render_map_inline()
   ◀────────── image/png bytes      限流检查 (30/min)

4. 点击导出
   POST /v1/recipes ─────────────▶  规范化 spec → spec_hash → 幂等创建
   POST /v1/render-jobs ─────────▶  幂等创建 + 配额预留
                                    → outbox_events: render.created.v1
   ◀────────── { jobId, status }

5. Worker 异步处理
   Outbox Worker poll ───────────▶  claim render.created.v1
     → ComputeClient.create_durable_run()
     → C++ 异步计算 (OpenMP/AVX2/CUDA)
     → outbox_events: render.poll.v1 (链式)

   Outbox Worker poll ───────────▶  claim render.poll.v1
     → 检查 Compute run 状态
     → running → reschedule (N 秒后重试)
     → completed → get_manifest → 选择产物 → outbox: media.create_derivatives
     → failed → 标记失败

   Media Worker ─────────────────▶  claim media.create_derivatives
     → 从 Compute 流式下载 master
     → 上传到 MinIO (master + thumbnail + watermarked_preview)
     → 创建 assets + asset_files 记录
     → render_job → completed

6. 用户查看/管理资产
   GET /v1/me/assets ────────────▶  含预签名预览 URL (MinIO)
   POST /v1/assets/{id}/download-url ──▶ 预签名下载 URL (5min TTL)

7. 挂牌出售
   POST /v1/listings ────────────▶  创建草稿 + 许可协议
   POST /v1/listings/{id}/publish ──▶ 创建不可变版快照 → 公开可见

8. 购买流程
   POST /v1/checkout ────────────▶  创建 order + payment_attempt
                                    → 生成支付宝表单 (或 Stub)
   ◀────────── { alipayForm }       前端 submitAlipayForm() 跳转

   POST /v1/webhooks/alipay ◀──── 支付宝异步通知 (或 Stub 模拟)
     → 签名验证 → payment_notifications → 对账
     → order fulfilled → entitlements 授权
     → ledger_entries (creator_credit + platform_fee)
     → 更新 creator_balances

9. 创作者提现
   POST /v1/me/payout-requests ──▶  上传 QR 码 → 金额校验
                                    每个 creator 最多一个 pending
   finance_operator:
   POST /internal/.../mark-paid ──▶  记录 external_reference
                                    → ledger_entries (payout_paid)
                                    → 更新 creator_balances
```

### 6.3 核心业务流程

**渲染链路（完整）：**
```
Next.js Studio Page
  → platform.studio.preview(spec)           // 实时预览
  → canonicalize_spec(spec)                 // 规范化
  → platform.studio.createRecipe(spec)      // 幂等创建配方
  → platform.studio.createRender(recipeId)  // 创建渲染任务
  → outbox: render.created.v1
  → ComputeClient.create_durable_run()      // 提交 C++ Compute
  → outbox: render.poll.v1 (链式)
  → ComputeClient.get_run_status()          // 轮询
  → ComputeClient.get_run_manifest()        // 获取产物清单
  → ComputeClient.stream_artifact()         // 流式下载
  → MinIO upload (master + derivatives)     // 上传对象存储
  → assets + asset_files 记录               // 资产入库
  → render_job → completed                  // 任务完成
  → 用户可下载/挂牌
```

**支付链路：**
```
Checkout → Order(pending_payment) + PaymentAttempt(created)
  → Alipay page redirect (或 Stub 模拟支付)
  → Alipay async notify → webhook → 签名验证 → 指纹去重
  → PaymentAttempt(succeeded) → Order(fulfilled)
  → Entitlement(active) → Ledger(creator_credit + platform_fee)
  → CreatorBalance(available += creator_amount)
```

### 6.4 优化改造方案

#### 阶段一：文档同步（立即）

1. **重写 `docs/architecture.md`**：反映 Next.js → FastAPI → C++ 三层架构，删除 Vue/Vite 引用
2. **更新 `docs/commercialization_implementation.md`**：进度快照对齐实际代码状态
3. **更新 `docs/API_INVENTORY.md`**：当前应称为 Platform API 文档而非 Legacy API

#### 阶段二：安全加固（短期）

1. **session_secret 启动校验**：本地开发也应至少 32 字节（Docker 已满足，裸机运行时无保护）
2. **Alipay webhook 签名文档化**：`payment_gateway.py` 的逻辑应补充合同文档
3. **前端受保护路由服务端检查**：middleware.ts 可增加 `fs_session` cookie 存在性校验，减少未登录页面闪烁
4. **CORS origins 解析增加启动时校验**：对 `split(",")` 结果做格式校验并记录日志

#### 阶段三：功能补全（中期）

1. **前端暴露 video/mesh 输出选择**：后端已全部就绪，是投入产出比最高的新功能（已知 #2）
2. **双产物摄取**：`_select_artifacts` 改为摄取全部必需产物，避免浪费已计算资源（已知 #7）
3. **`depthOctaves` 前端控件**：VideoOutputSpec 已定义，前端补充 slider 控件即可
4. **capabilities 按需降级**：Compute 不可用时应禁用控件而非用硬编码 fallback（已知 #10）

#### 阶段四：架构演进（长期）

1. **C++ HTTP server 升级**：从每连接 detached thread 改为线程池或 io_uring/epoll 事件驱动
2. **Compute kind 逐步接入**：special_points → raw_field → ln_map → 完整视频管线
3. **生产部署加固**：C++ 侧支持 TLS、支持 Unix domain socket、支持 cgroups 资源限制
4. **监控与可观测性**：结构化日志 → OpenTelemetry traces → 渲染指标仪表板
5. **plugin/extension 机制**：自定义 variant/formula 的安全编译沙箱（目前 `FSD_ENABLE_LEGACY_FORMULA_COMPILER=0` 禁用了 `g++ + dlopen` 方式）

---

*报告完毕。所有分析基于仓库内真实文件。*

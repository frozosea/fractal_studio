# Architecture / 架构

这份文档说明 Fractal Studio 的代码如何分层、请求如何流动，以及新增功能时应该改哪些地方。README 只保留项目入口；具体计算链路拆到各 pipeline 文档。

## Runtime Topology / 运行拓扑

开发时通常由 `dev.sh` 同时拉起两个进程：

```text
Browser
  -> Vue 3 / Vite frontend (:5174 from dev.sh, :5173 from npm run dev)
  -> frontend/src/api.ts
  -> native C++ HTTP backend (:18080)
  -> backend/src/api/routes_*.cpp
  -> backend/src/compute/* and backend/src/core/*
  -> runtime/runs/<category>/<runId>/ + runtime/db/fractal_studio.sqlite3
```

后端是一个本地原生 C++ HTTP 服务，不依赖 Web 框架。`frontend/src/api.ts` 默认请求 `http://<current-host>:18080`，也可以用 `VITE_BACKEND_URL` 覆盖。

## Backend Layers / 后端分层

| Layer | Files | Responsibility |
|---|---|---|
| Entry | `backend/src/main.cpp` | 定位项目根目录，创建 `runtime/`、SQLite DB、`JobRunner`，启动 HTTP server。 |
| HTTP core | `backend/src/core/http_server.cpp` | 解析 HTTP 请求、CORS、路由分发、每连接一个 detached thread。 |
| Runtime core | `backend/src/core/job_runner.cpp`, `db.cpp`, `resource_manager.cpp`, `path_guard.cpp`, `hardware_probe.cpp` | run 生命周期、进度、产物、持久化、路径安全、硬件探测。 |
| API routes | `backend/src/api/routes_*.cpp` | JSON 参数解析、调用计算模块、写入 artifact、返回前端需要的响应结构。 |
| Compute kernels | `backend/src/compute/*` | 分形渲染、ln-map、3D mesh、transition volume、special points、SIMD/CUDA/fixed-point 支持。 |
| Hardware adapters | `backend/src/adapters/*` | CUDA/OpenMP 探测等平台能力适配。 |

路由声明集中在 `backend/src/include/routes.hpp`。实际 URL 分发在 `backend/src/core/http_server.cpp`。

## Frontend Layers / 前端分层

| Layer | Files | Responsibility |
|---|---|---|
| App shell | `frontend/src/App.vue`, `components/NavRail.vue`, `components/StatusRail.vue` | 三栏桌面布局、移动端顶栏/底栏、全局状态展示。 |
| Routing | `frontend/src/router.ts` | `map`, `points`, `3d`, `runs`, `system` 页面路由。 |
| API client | `frontend/src/api.ts` | 后端请求封装、请求/响应类型、artifact URL 处理。 |
| Views | `frontend/src/views/*.vue` | 页面级状态、表单、工作流。 |
| Rendering components | `components/MapCanvas.vue`, `components/ThreeDViewer.vue`, `components/SpecialPointList.vue` | Canvas/Three.js/特殊点列表等重交互区域。 |
| Shared UI/state | `frontend/src/i18n.ts`, `theme.ts`, `device.ts`, `types.ts`, `assets/*.css` | 语言、主题、设备模式、共享类型和设计 token。 |

## Data And Artifact Lifecycle / 数据与产物生命周期

1. 前端 view 组装 typed request，通过 `api.ts` 发给 `/api/*`。
2. route 使用 `nlohmann::json` 解析参数，并选择对应 compute pipeline。
3. 会产生产物的任务通过 `JobRunner::createRun()` 创建 `runtime/runs/<category>/<runId>/`；`category` 由 module 映射到 `maps`、`videos`、`ln-maps`、`meshes`、`points` 等产品分类。
4. route 在计算过程中更新 `progress.json`，结束后调用 `addArtifact()` 写入 artifact 记录。
5. run 元数据持久化到 `runtime/db/fractal_studio.sqlite3`。
6. 前端通过 `/api/runs`, `/api/runs/status`, `/api/artifacts/*` 展示历史、进度和文件。

高频交互接口并不总是写 artifact。例如 `/api/map/render-inline` 返回二进制图像帧，`/api/map/field` 返回原始场数据，适合实时预览。

当前写入统一使用分类布局。为兼容已有数据，run 和 artifact 查询仍可读取旧的扁平布局 `runtime/runs/<runId>/`；无需迁移历史目录，新任务也不会继续写入该布局。

## Pipeline Deep Dives / 计算链路文档

- [render_pipeline.md](render_pipeline.md): 2D map、Julia、transition slice、raw field、engine/scalar、自定义公式。
- [special_points.md](special_points.md): center/Misiurewicz、Newton 求解、搜索、分类、持久化。
- [recurrence_metric.md](recurrence_metric.md): `min_pairwise_dist` 的公式、复杂度、回退策略和 HS-Recurrence 用法。
- [3d_pipeline.md](3d_pipeline.md): HS field/mesh、transition volume、marching cubes、voxel export。
- [video_pipeline.md](video_pipeline.md): ln-map、preview、统一视频导出、warp/encode。
- [testing.md](testing.md): 自动测试、构建检查、手动 QA 和提交前检查。

## Concurrency And Cancellation / 并发与取消

- HTTP server 对每个连接启动一个 detached thread，避免一个长请求阻塞所有请求。
- `JobRunner` 用 mutex 管理内存中的 run、progress、cancel flags，并把完成/失败/取消状态写到 SQLite。
- 部分长任务会调用 `setCancelable()` 并轮询 `isCancelRequested()`。
- 前端的高频 map 请求使用 preempt/request id 模型，避免慢响应覆盖新视图。

## Adding A Feature / 新增功能落点

1. 后端新增 route：在 `backend/src/include/routes.hpp` 声明，在 `http_server.cpp` 分发，在合适的 `routes_*.cpp` 实现。
2. 计算逻辑放进 `backend/src/compute/`，不要把重计算堆在 route 里。
3. 如果会生成文件，使用 `JobRunner` 创建 run、写进 `runtime/runs/<category>/<runId>/`，并注册 artifacts。
4. 前端先在 `frontend/src/api.ts` 增加类型和 client 函数，再接入对应 view/component。
5. 如果新增页面，更新 `frontend/src/router.ts` 和 `components/NavRail.vue`。
6. 新增移动端 UI 时同步检查 `frontend/src/device.ts`、`assets/base.css` 和目标 view 的 responsive block。

## Existing Documentation / 现有文档

- `README.md`: 项目简介、最短启动路径和文档索引。
- [docs/feature_status.md](feature_status.md): 已实现能力与明确暂缓的功能决策。
- `docs/development.md`: 本地开发、构建和排障。
- `docs/frontend.md`: 前端结构、移动端/平板适配策略。
- `docs/render_pipeline.md`: 二维渲染、引擎、变体和自定义公式。
- `docs/special_points.md`: 特殊点 solver、搜索和分类。
- `docs/recurrence_metric.md`: `min_pairwise_dist` / HS-Recurrence。
- `docs/3d_pipeline.md`: HS、transition volume、mesh 和 voxel。
- `docs/video_pipeline.md`: ln-map 和视频导出。
- `docs/testing.md`: 测试和 QA。
- `docs/lnmap_precision_experiments.md`: ln-map 精度实验记录。
- `HS_legacy_guide_zh.md`: legacy HS 学习笔记，主要用于理解历史实现。

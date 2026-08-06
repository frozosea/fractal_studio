# Fractal Studio / 分形工作室

Fractal Studio is a Platform product: Next.js browser UI, FastAPI Platform API, private C++ Compute, PostgreSQL, Redis and MinIO.

Fractal Studio 是一个本地运行的交互式分形实验室：后端负责原生 C++ 计算、产物管理和 HTTP API；当前商业前端优先提供二维图谱、Julia、轴向过渡、自定义轨道与 PNG 导出，三维和视频能力仍保留在 Compute 合同中供后续产品阶段接入。

## Quick Start / 快速启动

Recommended local development start:

```bash
./dev.sh
```

Docker development mode keeps Next.js hot reload and source mounts:

```bash
docker compose -f docker-compose.dev.yml up -d --build
```

Release mode prebuilds the frontend and runs `next start` without source mounts:

```bash
docker compose -f docker-compose.dev.yml -f docker-compose.release.yml up -d --build
```

Default URLs:

- Frontend: `http://localhost:3010`
- Platform API: `http://localhost:18100`
- Private Compute A: `http://localhost:18101`
- Private Compute B: `http://localhost:18104`

Manual backend/frontend commands, dependencies, runtime directories, and troubleshooting are in [docs/development.md](docs/development.md). Test and QA flow is in [docs/testing.md](docs/testing.md).

## Product path

```text
Browser -> Next.js (/platform/v1) -> FastAPI Platform -> private C++ Compute (/compute/v1)
```

Platform owns authentication, CSRF, assets, listings, orders, entitlements and
payouts. Compute is reachable only by the Platform worker with its service key.
Legacy C++ `/api/*` routes are not a browser dependency.

## Documentation / 文档

- [Architecture / 架构](docs/architecture.md): backend/frontend layers, data flow, compute pipelines, and where to add features.
- [Feature Status / 功能状态](docs/feature_status.md): dated implementation status and explicit deferral decisions.
- [Development Guide / 开发手册](docs/development.md): local setup, build commands, runtime directories, and troubleshooting.
- [Frontend Guide / 前端与移动端维护说明](docs/frontend.md): route groups and the public-page guard, visual languages, the shared card grid, touch and safe-area rules, map canvas gestures, and the mobile QA checklist.
- [Render Pipeline / 二维渲染链路](docs/render_pipeline.md): map render, Julia, transition slices, engines, scalars, variants, and custom formulas.
- [AI-Assisted Exploration / AI 辅助探索](docs/ai_assisted_exploration.md): 找位置、调色、构图微调、文案与上架的经验、工作流和质量门。
- [Commercialization Implementation / 商业化实施记录](docs/commercialization_implementation.md): FastAPI/Compute v1 商业化重构计划、当前完成度与验证记录。
- [Compute Backend / 计算后端](backend/README.md): 私有服务构建、配置、测试、运行目录和生产安全边界。
- [Compute v1 Contract / 私有计算合同](docs/compute_v1_contract.md): 服务后端实现所需的鉴权、transport DTO、状态机、manifest、下载、硬件证据和错误合同。
- [Compute v1 Cookbook / 从零调用手册](docs/compute_v1_cookbook.md): Key 生成、workload 选择、curl、DSL/Orbit sequence 和 transition 请求示例。
- [Coloring Contract / 染色合同](docs/coloring_contract.md): 内置染色字段、自定义 gradient schema、支持矩阵及 Platform/前端接入任务。
- [Compute v1 Jobs / 任务参数与产物](docs/compute_v1_jobs.md): 19 个 kind 的 payload 默认值、限制、preview 结构和必需 artifact。
- [Platform–Compute Integration / 服务后端对接指南](docs/platform_compute_integration.md): FastAPI ComputeClient、PostgreSQL Outbox、轮询/取消、产物摄取和硬件策略。
- [Hybrid Cloud Compute Deployment / 云端业务层与双计算节点计划](docs/hybrid_cloud_compute_deployment_plan.md): VPS 控制面、OSS/CDN、4090 主节点、GTX 1050 辅助节点、调度亲和及按时长/token 估算的迁移计划。
- [Special Points / 特殊点链路](docs/special_points.md): center/Misiurewicz solving, search, classification, progress, and artifacts.
- [Recurrence Metric / 递归距离度量](docs/recurrence_metric.md): `min_pairwise_dist`, HS-Recurrence behavior, cost, and supported engines.
- [3D Pipeline / 三维链路](docs/3d_pipeline.md): HS fields/meshes, transition volumes, marching cubes, and voxel export.
- [Video Pipeline / 视频链路](docs/video_pipeline.md): ln-map, preview frames, unified export, warp/encode, progress, and artifacts.
- [Testing / 测试](docs/testing.md): backend tests, frontend build checks, manual QA, and pre-commit checklist.
- [ln-map Precision Experiments / 精度实验](docs/lnmap_precision_experiments.md): notes on `fp32` / `fp64` / `fx64` precision and speed.

## Repository Map / 仓库速览

```text
backend/   Native C++ HTTP API and compute kernels
frontend/  Next.js browser user interface
docs/      Architecture, development, pipeline, frontend, and QA docs
runtime/   Local build output, logs, run artifacts, and SQLite DB
scripts/   System and legacy integrity checks
```

## License / 许可

MIT License.

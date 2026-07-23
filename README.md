# Fractal Studio / 分形工作室

Fractal Studio is a local fractal exploration app with a native C++ compute backend and a Vue 3/Vite frontend.

Fractal Studio 是一个本地运行的交互式分形实验室：后端负责原生 C++ 计算、产物管理和 HTTP API，前端提供地图探索、Julia、3D、视频导出和系统诊断界面。

## Quick Start / 快速启动

Recommended local development start:

```bash
./dev.sh
```

Default URLs:

- Frontend: `http://localhost:5174`
- Backend: `http://localhost:18080`

Manual backend/frontend commands, dependencies, runtime directories, and troubleshooting are in [docs/development.md](docs/development.md). Test and QA flow is in [docs/testing.md](docs/testing.md).

## Startup Benchmark / 启动校准

In server mode, the backend synchronously calibrates its compute engines before opening the HTTP port. The default `quick` mode measures two scheduler workloads: an interactive `256×256`, 1000-iteration map and a batch `512×512`, 2000-iteration map. Each engine/scalar pair gets one warmup and two measured samples; `full` uses three measured samples. The local `export-map` and `export-video` CLI commands skip startup calibration.

后端在服务模式下会先同步校准计算引擎，再开放 HTTP 端口。默认的 `quick` 模式包含交互地图和批处理地图两档，使自动调度有当前机器上的实测速率作为参考。可通过环境变量选择模式：

```bash
# Default: one warmup + two samples per engine/scalar pair
FSD_STARTUP_BENCHMARK=quick ./dev.sh

# One warmup + three samples for a steadier reference
FSD_STARTUP_BENCHMARK=full ./dev.sh

# Start immediately and use capability-based fallback scheduling
FSD_STARTUP_BENCHMARK=off ./dev.sh
```

`0` and `false` are aliases for `off`. An empty value uses `quick`; an unsupported value prints a warning and also falls back to `quick`. A failed or partially unavailable calibration is reported in the backend log, but it does not prevent the service from starting: missing measurements use the capability-based fallback.

`POST /api/benchmark` can refresh the in-process scheduler reference while the service is running. `replaceCache: true` atomically replaces the current reference with the submitted profile; `replaceCache: false` merges results by compute family, workload, work size, engine, and scalar. For example:

```bash
curl -X POST http://localhost:18080/api/benchmark \
  -H 'Content-Type: application/json' \
  -d '{"workload":"interactive","width":256,"height":256,"iterations":1000,"warmup":1,"samples":3,"replaceCache":true}'
```

The active reference is observable at `GET /api/system/capabilities` under `benchmarkCache` (`available` plus the measured `results`). The startup log also reports each workload's duration, the total calibration time, and any fallback reason.

## Documentation / 文档

- [Architecture / 架构](docs/architecture.md): backend/frontend layers, data flow, compute pipelines, and where to add features.
- [Feature Status / 功能状态](docs/feature_status.md): dated implementation status and explicit deferral decisions.
- [Development Guide / 开发手册](docs/development.md): local setup, build commands, runtime directories, and troubleshooting.
- [Frontend Guide / 前端与移动端维护说明](docs/frontend.md): frontend structure, responsive strategy, tablet-landscape behavior, and mobile QA checklist.
- [Render Pipeline / 二维渲染链路](docs/render_pipeline.md): map render, Julia, transition slices, engines, scalars, variants, and custom formulas.
- [Commercialization Implementation / 商业化实施记录](docs/commercialization_implementation.md): FastAPI/Compute v1 商业化重构计划、当前完成度与验证记录。
- [Compute Backend / 计算后端](backend/README.md): 私有服务构建、配置、测试、运行目录和生产安全边界。
- [Compute v1 Contract / 私有计算合同](docs/compute_v1_contract.md): 服务后端实现所需的鉴权、transport DTO、状态机、manifest、下载、硬件证据和错误合同。
- [Compute v1 Jobs / 任务参数与产物](docs/compute_v1_jobs.md): 18 个 kind 的 payload 默认值、限制、preview 结构和必需 artifact。
- [Platform–Compute Integration / 服务后端对接指南](docs/platform_compute_integration.md): FastAPI ComputeClient、PostgreSQL Outbox、轮询/取消、产物摄取和硬件策略。
- [Special Points / 特殊点链路](docs/special_points.md): center/Misiurewicz solving, search, classification, progress, and artifacts.
- [Recurrence Metric / 递归距离度量](docs/recurrence_metric.md): `min_pairwise_dist`, HS-Recurrence behavior, cost, and supported engines.
- [3D Pipeline / 三维链路](docs/3d_pipeline.md): HS fields/meshes, transition volumes, marching cubes, and voxel export.
- [Video Pipeline / 视频链路](docs/video_pipeline.md): ln-map, preview frames, unified export, warp/encode, progress, and artifacts.
- [Testing / 测试](docs/testing.md): backend tests, frontend build checks, manual QA, and pre-commit checklist.
- [ln-map Precision Experiments / 精度实验](docs/lnmap_precision_experiments.md): notes on `fp32` / `fp64` / `fx64` precision and speed.

## Repository Map / 仓库速览

```text
backend/   Native C++ HTTP API and compute kernels
frontend/  Vue 3/Vite user interface
docs/      Architecture, development, pipeline, frontend, and QA docs
runtime/   Local build output, logs, run artifacts, and SQLite DB
scripts/   System and legacy integrity checks
```

## License / 许可

MIT License.

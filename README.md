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

## Documentation / 文档

- [Architecture / 架构](docs/architecture.md): backend/frontend layers, data flow, compute pipelines, and where to add features.
- [Development Guide / 开发手册](docs/development.md): local setup, build commands, runtime directories, and troubleshooting.
- [Frontend Guide / 前端与移动端维护说明](docs/frontend.md): frontend structure, responsive strategy, tablet-landscape behavior, and mobile QA checklist.
- [Render Pipeline / 二维渲染链路](docs/render_pipeline.md): map render, Julia, transition slices, engines, scalars, variants, and custom formulas.
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

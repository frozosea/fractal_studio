# Development Guide / 开发手册

这份文档记录本地开发、构建和排障流程。README 是项目入口；架构说明在 `docs/architecture.md`，测试与 QA 细节在 `docs/testing.md`。

## Requirements / 依赖

Backend:

- CMake 3.18+
- C++20 compiler
- OpenCV components: `core`, `imgcodecs`, `imgproc`, `videoio`
- sqlite3
- Linux/POSIX runtime libraries
- Optional: OpenMP, CUDA toolkit, `ffmpeg`, runtime `g++` for custom formulas

Frontend:

- Node.js 18+
- npm

## Recommended Dev Start / 推荐启动方式

在项目根目录运行：

```bash
./dev.sh
```

默认端口：

- Backend: `http://localhost:18080`
- Frontend: `http://localhost:5174`

`dev.sh` 会做这些事：

- 用 Release 配置构建 backend 到 `runtime/build/`
- 如果端口被旧进程占用，用 `fuser` 释放 backend/frontend 端口
- 从仓库根目录启动 backend
- 从 `frontend/` 启动 Vite，并开启 `--host`
- 写日志到 `runtime/logs/backend.log` 和 `runtime/logs/frontend.log`
- Ctrl-C 时同时关闭两个进程

自定义端口：

```bash
./dev.sh --backend-port 18081 --frontend-port 5175
```

如果前端访问的后端不是当前 host 的 `18080`，需要设置：

```bash
VITE_BACKEND_URL=http://<backend-host>:18080 npm run dev
```

## Manual Backend Build / 手动构建后端

```bash
make backend-build
./runtime/build/fractal_studio_backend 18080
```

等价 CMake 命令：

```bash
cmake -S backend -B runtime/build -DCMAKE_BUILD_TYPE=Release
cmake --build runtime/build -j
./runtime/build/fractal_studio_backend 18080
```

后端启动时会向上查找包含 `backend/CMakeLists.txt` 和 `frontend/package.json` 的 Fractal Studio 根目录。因此推荐从项目根目录运行 backend。

## Frontend Commands / 前端命令

第一次安装：

```bash
cd frontend
npm install
```

开发：

```bash
cd frontend
npm run dev
```

生产构建和预览：

```bash
cd frontend
npm run build
npm run preview
```

注意：`npm run dev` 使用 `frontend/vite.config.ts` 中的默认 `5173`；`dev.sh` 会显式改用 `5174`，避免和其它 Vite 项目撞端口。

## Tests And Checks / 测试与检查

完整测试和 QA 流程见 [`docs/testing.md`](testing.md)。常用命令如下：

Backend smoke test:

```bash
cmake -S backend -B runtime/build -DCMAKE_BUILD_TYPE=Release
cmake --build runtime/build -j
ctest --test-dir runtime/build --output-on-failure
```

Frontend build check:

```bash
cd frontend
npm run build
```

System helper targets:

```bash
make verify-system
make verify-immutable
```

`verify-system` 当前会检查 `nvcc`，所以它比 CPU-only 后端构建更严格。没有 CUDA 的机器仍然可以构建 CPU 后端。

## Runtime Directories / 运行时目录

| Path | Purpose |
|---|---|
| `runtime/build/` | CMake backend build output. |
| `runtime/logs/` | `dev.sh` backend/frontend logs. |
| `runtime/runs/<runId>/` | PNG/MP4/GLB/STL/report/progress artifacts. |
| `runtime/db/fractal_studio.sqlite3` | Run and artifact metadata. |
| `../runs/custom_variants/` | Custom formula shared library cache in the current checkout layout. |
| `../fractal_studio.db` | Custom variant registry in the current checkout layout. |

`runId` 使用 `yymmdd-hhmmss`，同一秒内多个任务会追加短数字后缀。

## Useful Local URLs / 常用地址

```text
Frontend: http://localhost:5174
Backend health: http://localhost:18080/api/system/check
Hardware: http://localhost:18080/api/system/hardware
Capabilities: http://localhost:18080/api/system/capabilities
Runs: http://localhost:18080/api/runs
```

## Troubleshooting / 排障

### Frontend opens but API calls fail

- 确认 backend 还在运行：`http://localhost:18080/api/system/check`
- 如果手机或平板访问电脑上的 Vite 地址，前端默认会请求 `http://<phone-visible-host>:18080`。电脑防火墙、局域网 IP、后端端口都要可达。
- 需要手动指定后端时，用 `VITE_BACKEND_URL=http://<server-ip>:18080 npm run dev -- --host 0.0.0.0`。

### Backend build fails on CUDA

- CUDA 是可选能力。CMake 找不到 CUDA 时会禁用 GPU kernels。
- CUDA 13 不再支持 Pascal `sm_61` 离线编译；需要 GTX1050/Pascal 二进制时使用 CUDA 12.x，或调整 `CMAKE_CUDA_ARCHITECTURES`。

### MP4 export fails

- 确认系统有 `ffmpeg`。
- 查看 `runtime/logs/backend.log` 和对应 `runtime/runs/<runId>/progress.json`。

### Custom formula fails

- 确认运行时能找到 `g++`。
- 公式会被编译成 `.so` 并用 `dlopen` 加载；删除失败缓存后可重新编译。
- 这个能力只适合本地可信输入。

### Mobile browser shows old layout

- 确认访问的是最新 Vite dev server，不是旧 tab 或旧端口。
- 强制刷新后检查 `frontend/src/device.ts` 是否把设备判为 `mobile`，以及 `<html>` 是否有 `data-device="mobile"`。
- 平板横屏的关键范围是 `761px <= width <= 1200px` 且 `orientation: landscape`。

## Before Committing / 提交前建议

对后端改动，至少跑：

```bash
make backend-build
ctest --test-dir runtime/build --output-on-failure
```

对前端改动，至少跑：

```bash
cd frontend
npm run build
```

只改文档时，可以用：

```bash
git diff --check
```

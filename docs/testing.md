# Testing / 测试

这份文档记录当前项目可用的构建、测试和手动验证流程。

## Backend Build / 后端构建

```bash
cmake -S backend -B runtime/build -DCMAKE_BUILD_TYPE=Release
cmake --build runtime/build -j
```

等价 Makefile target：

```bash
make backend-build
```

CUDA 是可选能力。CMake 找不到 CUDA 时会禁用 GPU kernels，CPU backend 仍可构建。

## Backend Tests / 后端测试

```bash
ctest --test-dir runtime/build --output-on-failure
```

当前 CMake 注册的测试：

- `special_points_smoke`
- `compute_path_diff`

测试源文件：

- `backend/src/tests/special_points_smoke.cpp`
- `backend/src/tests/compute_path_diff.cpp`

覆盖重点：

- center period expected count
- Misiurewicz `(preperiod=2, period=1)`，根应为 `c = -2`
- 高周期 local center Newton 收敛
- 深 zoom local center search
- viewport sampled search

## Compute Path Differential Tester / 计算路径对拍器

`compute_path_diff` 会用同一 scalar 的 OpenMP 路径作为基准，对 AVX2、AVX512、CUDA 等可用 map render 路径做 BGR 图像差分。不同 scalar 不直接拿深 zoom 互相比，因为 `fp32`、`fp64`、`fx64` 的舍入模型不同，深度大时结果本来就可能不同。

默认 quick 套件覆盖：

- Mandelbrot escape
- Burning Ship `min_abs`
- Julia escape
- `fp64`、`fp32`、`fx64`
- AVX2 / AVX512 / CUDA 可用时自动对拍；不可用时记录 `SKIP`

跨 scalar 只跑专门构造的 fp32-equivalent 场景：

- 坐标、scale、Julia 参数都是二进制可精确表示的 dyadic 值。
- iteration 很小，避免不同精度的合法舍入差异扩散。
- colormap 使用只依赖整数 iteration 的 `mod17`。
- 基准是 `openmp/fp32`；`openmp/fp64` 和 `openmp/fx64` 必须与它逐像素完全一致。

直接运行：

```bash
./runtime/build/compute_path_diff
```

通过 CTest 运行：

```bash
ctest --test-dir runtime/build -R compute_path_diff --output-on-failure
```

测评机可以打开硬件覆盖要求。对应路径没有实际跑到时，测试会失败：

```bash
FSD_DIFF_EXPECT_AVX2=1 \
FSD_DIFF_EXPECT_AVX512=1 \
FSD_DIFF_EXPECT_CUDA=1 \
ctest --test-dir runtime/build -R compute_path_diff --output-on-failure
```

Hybrid 需要足够大的 workload 才会进入真实 CPU+GPU tile scheduler，默认 quick 套件不跑。测评机可开启慢测：

```bash
FSD_DIFF_INCLUDE_SLOW=1 \
FSD_DIFF_EXPECT_HYBRID=1 \
ctest --test-dir runtime/build -R compute_path_diff --output-on-failure
```

输出字段：

- `actual=<engine>/<scalar>`: 后端实际走到的路径。
- `max`: 单通道最大像素差。
- `mean`: 所有 BGR 通道平均绝对差。
- `bad`: 超过阈值的像素比例。
- `SKIP`: 请求路径不可用或回退。
- `FAIL`: 差异超过阈值，或测评机要求的路径未覆盖。

## Frontend Build Check / 前端构建检查

```bash
cd frontend
npm run build
```

这个检查能覆盖 TypeScript/Vite/Vue 编译层面的错误，但不等于视觉回归测试。

## Dev Server Smoke / 本地联调冒烟

```bash
./dev.sh
```

默认地址：

```text
Frontend: http://localhost:5174
Backend: http://localhost:18080
```

Backend smoke URLs：

```text
http://localhost:18080/api/system/check
http://localhost:18080/api/system/hardware
http://localhost:18080/api/system/capabilities
http://localhost:18080/api/runs
```

## Manual Feature Checks / 手动功能检查

### 2D Render

- 打开 Map 页面。
- 切换 variant、metric、colormap。
- 拖拽/缩放，确认旧图不会覆盖新视图。
- 试一次 still export，确认 `runtime/runs/<runId>/map.png` 存在。

### Recurrence Metric

- 在 Map 或 3D 中选择 `min_pairwise_dist`。
- 观察响应中的 `engineUsed` 是否回退到 OpenMP。
- 降低 resolution 后再提高 iterations 或 pairwise cap。

### Special Points

- 在 Map 页面打开 special points panel。
- 执行 viewport search。
- 确认返回点有 `accepted=true`、`actual.period` 和 `compatibleVariants`。
- Runs 页面应能看到 search artifact。

### 3D

- 生成 HS field，确认 viewer 有稳定尺寸且不空白。
- 生成 HS mesh，确认 GLB/STL artifact 可下载。
- 生成 transition voxels，确认 `faceCount > 0`。
- 生成 transition mesh，确认 `vertexCount` 和 `triangleCount` 非零。

### Video

- 先跑 `/api/video/preview` 对应的 UI 预览。
- 再跑完整 export。
- Runs 页面确认 `ln_map.png`、`final_frame.png`、`start_frame.png`、`end_frame.png`、`zoom.mp4`、`video_export.json` 都存在。
- 查看 progress stage 是否从 `queued` 到 `completed`。

## Mobile And Tablet QA / 移动端和平板检查

移动端布局细节见 [frontend.md](frontend.md)。每次动前端布局至少检查：

- phone portrait: `390x844` 或 `412x915`
- phone landscape: `844x390`
- tablet landscape: `1024x768`
- large tablet landscape: `1180x820`

重点：

- 顶部导航不遮挡内容。
- StatusRail 移动端默认折叠。
- Map canvas 可见且可拖拽/缩放。
- 控制区局部滚动，不让整个 app 横向溢出。
- 平板横屏不是桌面三栏老布局。

## Pre-commit Checklist / 提交前建议

后端代码改动：

```bash
make backend-build
ctest --test-dir runtime/build --output-on-failure
```

前端代码改动：

```bash
cd frontend
npm run build
```

文档改动：

```bash
git diff --check
```

## Known Gaps / 已知缺口

- 目前没有自动浏览器视觉回归测试。
- 没有 Playwright 断点截图检查，移动端布局仍依赖人工 QA。
- 3D canvas 和 video export 更依赖手动验证，因为产物较大、运行时间较长。

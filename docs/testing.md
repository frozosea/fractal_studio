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

测试源文件：

- `backend/src/tests/special_points_smoke.cpp`

覆盖重点：

- center period expected count
- Misiurewicz `(preperiod=2, period=1)`，根应为 `c = -2`
- 高周期 local center Newton 收敛
- 深 zoom local center search
- viewport sampled search

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

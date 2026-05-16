# 3D Pipeline / 三维链路

这份文档说明 HS heightfield、transition volume、marching cubes、voxel preview 和 mesh artifact 的实现链路。

## Entry Points / 入口

| Endpoint | Purpose |
|---|---|
| `POST /api/hs/field` | 返回 HS raw field，前端用 Three.js 自己生成高度场。 |
| `POST /api/hs/mesh` | 后端生成 HS GLB/STL mesh artifact。 |
| `POST /api/transition/mesh` | 构建 3D transition volume，再 marching cubes 输出 GLB/STL。 |
| `POST /api/transition/voxels` | 构建 transition volume，提取 Minecraft-style voxel faces，并输出 STL + base64 face arrays。 |

主要代码：

- `backend/src/api/routes_mesh.cpp`
- `backend/src/compute/hs/heightfield_mesh.hpp`
- `backend/src/compute/hs/heightfield_mesh.cpp`
- `backend/src/compute/transition_volume.hpp`
- `backend/src/compute/transition_volume.cpp`
- `backend/src/compute/transition_volume_avx2.cpp`
- `backend/src/compute/cuda/transition_volume.cu`
- `backend/src/compute/marching_cubes.cpp`
- `backend/src/compute/mesh_io.cpp`
- `frontend/src/views/ThreeDView.vue`
- `frontend/src/components/ThreeDViewer.vue`

## HS Heightfield / HS 高度场

HS field 复用 `escape_time.hpp` 的 per-pixel iteration core，把某个 metric 映射成高度：

| Metric | Height value |
|---|---|
| `escape` | escaped iteration / max iterations，bounded 为 `1.0`。 |
| `min_abs` | orbit 最小模长，HS-Base。 |
| `max_abs` | orbit 最大模长。 |
| `envelope` | `0.5 * (min_abs + max_abs)`。 |
| `min_pairwise_dist` | orbit 两两最近距离，HS-Recurrence，见 [recurrence_metric.md](recurrence_metric.md)。 |

`/api/hs/field` 只返回 raw float64 field 和 min/max，适合前端快速改变 z-scale、材质和相机。

`/api/hs/mesh` 会：

```text
computeHsField()
  -> clamp raw values
  -> normalize field to [0, 1]
  -> build uniform grid mesh
  -> add side walls and bottom
  -> write hs_mesh.glb and hs_mesh.stl
```

## Transition Volume / 过渡体数据

Transition volume 在三维空间中迭代两个 quadratic/folded variants 的桥接动力系统。

体素 `(x0, y0, z0)` 作为 seed，每一步计算：

```text
nx = real_projection(from, x^2, y^2)
   + real_projection(to,   x^2, z^2)
   - x^2 + x0

ny = imag_projection(from, x, y) + y0
nz = imag_projection(to,   x, z) + z0
```

`from_variant` 和 `to_variant` 默认是 Mandelbrot -> Burning Ship。当前 transition volume 主要支持前 10 个 quadratic/folded variants。

Field value 约定：

- inside or bounded: 小于 `0.5`
- escaped: `0.5..1.0`
- marching cubes 默认 `iso = 0.5`
- voxel preview 默认 `iso = 0.48`

## Transition Mesh / 过渡网格

`/api/transition/mesh` 流程：

```text
buildTransitionVolume()
  -> McField<float>
  -> marchingCubes(field, iso)
  -> write transition_mesh.glb
  -> write transition_mesh.stl
```

响应包含：

- `vertexCount`
- `triangleCount`
- `fieldMs`
- `mcMs`
- `fieldEngineUsed`
- `fieldScalarUsed`
- artifact URLs

## Voxel Preview / 体素预览

`/api/transition/voxels` 流程：

```text
buildTransitionVolume()
  -> classify inside voxel by iso
  -> emit only faces adjacent to outside voxels
  -> write transition_voxels.stl
  -> return base64 face arrays
```

返回的 base64 arrays：

- `posB64`: `float32[faceCount * 4 * 3]`
- `normB64`: `int8[faceCount * 3]`
- `depthB64`: `uint8[faceCount]`

这样前端只收到可见边界面，不需要传完整 `N^3` volume。

## Resource And Memory Guards / 资源保护

Transition volume 是重任务，route 会申请 resource locks：

```text
transition_volume
cuda_heavy
cpu_heavy
```

同一时间已有 transition volume 时，新请求返回 `409`，并带 `activeRunId` 和冲突 lock。

resolution 限制：

- transition mesh/voxels 总范围 `4..1024`
- `resolution >= 512` 默认禁用，需要 `allowLargeVolume=true`
- 大 volume 会估算 VRAM/内存，超过安全预算会拒绝

默认 resolution：

- voxel preview: `128`
- mesh: low-end CUDA 机器 `128`，其它机器 `192`

## Engine Selection / 引擎选择

`buildTransitionVolume()` 支持：

- `openmp`
- `avx2`
- `cuda`
- `hybrid`
- `auto`

`avx512` 当前会回退到 OpenMP。`auto` 会根据 work size、CUDA 可用性、AVX2/FMA 能力选择路径。大任务且非 low-end CUDA 机器优先 hybrid。

## Frontend Notes / 前端注意

`ThreeDView.vue` 同时处理 HS 和 transition 两类数据。`ThreeDViewer.vue` 负责 Three.js 展示。

移动端和平板横屏布局细节见 [frontend.md](frontend.md)。3D canvas 容器必须有稳定尺寸，否则 Three.js 初始化时容易拿到 0 宽高。

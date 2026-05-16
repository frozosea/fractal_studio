# Special Points / 特殊点链路

这份文档说明 Mandelbrot special points 的搜索、枚举、Newton 求解、分类、持久化和前端展示链路。

## Concepts / 概念

当前 special points 以二次 Mandelbrot critical orbit 为基础：

```text
z_0 = 0
z_{n+1} = z_n^2 + c
```

支持两类点：

- `center`: hyperbolic component center。critical orbit 是纯周期，`preperiod = 0`。
- `misiurewicz`: preperiodic point。critical orbit 先走 `m` 步，再进入周期 `p`。

求解后会用 orbit classifier 复核实际 `preperiod`、`period` 和 residual，避免 Newton 落到相邻根或低周期根。

## Entry Points / 入口

| Endpoint | Purpose |
|---|---|
| `POST /api/special-points/auto` | legacy polynomial auto solve，按 `(k, p)` 枚举根并持久化。 |
| `POST /api/special-points/seed` | 从用户给定 seed Newton 收敛到根并持久化。 |
| `POST /api/special-points/enumerate` | 按 period/preperiod 范围枚举 special points，异步 run。 |
| `POST /api/special-points/search` | 在当前 viewport 内搜索可见 special points，异步 run。 |
| `GET /api/special-points/results` | 读取 search/enumerate 的 JSON artifact 或进度。 |
| `POST /api/special-points/snap` | 把一个 seed snap 到附近 center。 |
| `GET /api/special-points` | 读取已持久化的 special points。 |

主要代码：

- `backend/src/api/routes_points.cpp`
- `backend/src/compute/special_points.hpp`
- `backend/src/compute/special_points.cpp`
- `backend/src/compute/newton/mandelbrot_sp.hpp`
- `backend/src/compute/newton/mandelbrot_sp.cpp`
- `backend/src/compute/cuda/special_points.cu`
- `backend/src/tests/special_points_smoke.cpp`
- `frontend/src/components/SpecialPointList.vue`
- `frontend/src/views/PointsView.vue`
- `frontend/src/views/MapView.vue`

## Modern Solver Flow / 当前 solver 流程

```text
routes_points.cpp
  -> parse kind/ranges/viewport/tolerances
  -> validate request size
  -> JobRunner createRun()
  -> compute::enumerate_special_points() or compute::search_special_points()
  -> Newton solve center / Misiurewicz
  -> classify critical orbit
  -> classify variant compatibility
  -> write special_points_search.json or special_points_enumerate.json
  -> artifact + progress response
```

`enumerate` 目标是找完整范围内的根。它先用 anchors，再用 deterministic low-discrepancy disk seeds，按 batch 并行尝试。

`search` 目标是在当前 viewport 里快速找可见点。它按 period/preperiod 递进，中心点搜索会结合 viewport-local seed 和 fallback candidate。大 period 的本地中心搜索是这个路径的核心使用场景。

## Legacy Polynomial Solver / legacy 多项式 solver

`compute/newton/mandelbrot_sp.*` 是旧 Python special point solver 的 native port。

它构造：

```text
f_n(c) = P_n(0; c)
g(0, p)(c) = f_{p-1}(c)
g(k > 0, p)(c) = f_{k+p-1}(c) + f_{k-1}(c)
```

然后试除低周期因子，用 Newton + deflation 找 roots。这个路径适合 modest `(k, p)`，高阶时多项式 degree 会变得不实际。当前更复杂的 viewport search 主要走 `special_points.cpp`。

## Request Limits / 请求限制

Route 层会限制枚举规模，避免一次请求拖垮本地服务：

- center enumerate: period `1..10`
- Misiurewicz enumerate: preperiod `1..6`，period `1..6`，且 `preperiod + period <= 10`
- expected count 上限：`3000`
- `maxNewtonIter`: `1..80`
- `maxSeedBatches`: `1..200`
- `seedsPerBatch`: `1..10000`

Local search 的内部上限更高，用于深 zoom 高周期中心：

- center period 上限可到 `8192`
- Misiurewicz preperiod/period 总量受本地常量约束
- viewport 是 search 的必需输入

## Result Shape / 结果结构

每个点会返回：

- `kind`, `preperiod`, `period`
- `re`, `im`
- `converged`, `accepted`, `fallback`, `visible`
- `residual`, `newtonIterations`
- `actual`: orbit classifier 输出
- `variants`: 不同 quadratic variant 下是否存在、是否同 orbit
- `compatibleVariants` / `variantCompatibility`
- `reason`: 接受或拒绝原因

前端不要只看 `converged`，真正可用点应看 `accepted`，可见过滤看 `visible`。

## Variant Compatibility / 变体兼容性

`classify_variant_existence()` 会用 Mandelbrot critical orbit 作为参考，并在前 10 个 quadratic/folded variants 中检查：

- variant 下 orbit 是否存在
- 是否和 Mandelbrot orbit 同步
- 实际 preperiod/period
- repeat error

这个结果用于 UI 告诉用户某个 Mandelbrot special point 在其它 folded variants 里是否仍然有同构意义。

## Progress And Artifacts / 进度与产物

`enumerate` 和 `search` 都通过 `JobRunner`：

- run type: `special-points-enumerate` 或 `special-points-search`
- progress fields: `stage`、`acceptedCount`、`expectedCount`、`seedCount`、`period`
- artifact: `special_points_search.json` 或对应 enumerate JSON

`GET /api/special-points/results?runId=<id>` 优先读取 artifact；如果 artifact 还没写完，则返回当前 progress。

## Testing / 测试

Smoke test 在 `backend/src/tests/special_points_smoke.cpp`，覆盖：

- center period 计数
- Misiurewicz `(m=2, p=1)`，应找到 `c = -2`
- 高周期本地中心收敛
- 深 zoom viewport search
- viewport sampled search

运行方式见 [testing.md](testing.md)。

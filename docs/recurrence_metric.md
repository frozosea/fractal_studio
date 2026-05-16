# Recurrence Metric / 递归距离度量

这份文档专门解释 `min_pairwise_dist`，也就是 HS-Recurrence 使用的 orbit recurrence metric。

## Definition / 定义

对同一个 orbit 中的点计算两两距离，取最小值：

```text
min_pairwise_dist = min |z_i - z_j|, i < j
```

当前实现比较的是迭代后产生的 orbit samples，也就是 `z_1..z_N`。普通 Mandelbrot 模式下：

```text
z_0 = 0
z_{n+1} = z_n^2 + c
```

Julia 模式下：

```text
z_0 = pixel coordinate
c = fixed Julia parameter
```

这个 metric 小，说明 orbit 在有限步内靠近了自己，适合显示 recurrence、近周期和隐藏结构。

## Implementation / 实现

核心函数：

- `backend/src/compute/escape_time.hpp::iterate_pairwise()`
- `backend/src/compute/map_kernel.cpp`
- `backend/src/compute/transition_kernel.cpp`
- `backend/src/compute/hs/heightfield_mesh.cpp`

`iterate_pairwise()` 每步做：

```text
z = step(z, c)
for prior in orbit_scratch:
    best = min(best, |z - prior|)
orbit_scratch.push_back(z)
stop if z escapes
```

返回值放在 `IterResult.extra`。

## Complexity / 复杂度

复杂度是：

```text
O(P^2) per pixel
P = min(iterations, pairwiseCap)
```

默认 `pairwiseCap = 64`。这是为了避免 `iterations` 很大时直接变成不可交互的 O(iterations^2)。

在 HS heightfield 中，当前实现固定使用 64 作为 pairwise cap。二维 map 和 transition slice 通过 `pairwiseCap` 参数传入。

## Supported Paths / 支持路径

| Path | Support |
|---|---|
| OpenMP / fp64 | 支持。 |
| Custom variants | 仅普通 map 的完整 CPU 路径能覆盖自定义公式，其它加速路径不覆盖。 |
| fixed-point | 不直接支持；会回退到 fp64 CPU 路径。 |
| AVX2 / AVX512 | 不支持 O(N^2) orbit buffer；回退。 |
| CUDA map kernel | 不支持 metric 4；回退。 |
| raw field | 支持，返回 `fieldB64` + min/max。 |
| HS mesh/field | 支持，是 HS-Recurrence 的主要来源。 |
| transition slice | 支持 CPU 计算。 |

`engine_select.cpp` 明确把 fixed-point + `MinPairwiseDist` 限制在 OpenMP 路径。

## Color And Field Behavior / 配色与 field

二维 map 中，`min_pairwise_dist` 作为 field metric 走 `colorize_field_bgr()`，不是 escape-time coloring。

`/api/map/field` 对非 escape metric 返回：

- `fieldB64`: `float64[width * height]`
- `fieldMin`
- `fieldMax`

前端可以在不重新计算 fractal 的情况下改 colormap。

HS pipeline 中：

- raw value 会被 clamp 到 `heightClamp`
- mesh 阶段再把 field normalize 到 `[0, 1]`
- z 坐标乘 `heightScale`

## Practical Guidance / 使用建议

- 交互预览不要把 `pairwiseCap` 拉太高。64 已经是比较贵的默认值。
- 如果画面噪声很强，先降低 iterations 或 resolution，再增加 pairwiseCap。
- 做 3D HS mesh 时优先调 `resolution` 和 `heightClamp`，再动 iterations。
- 如果用户选了 CUDA/AVX 但响应显示 OpenMP，这是正常回退，不是 bug。

## Known Risks / 已知风险

- 高 iteration + 高 resolution 会非常贵。
- Orbit 接近自身时小数值对 colormap 很敏感，field normalization 容易被极端值影响。
- Transcendental variants 可能快速产生非有限值，escape 会提前终止 pairwise 比较。

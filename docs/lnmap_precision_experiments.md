# ln-map Precision Experiments

This document records the local experiments used to evaluate `fp32`, `fp64`,
and `fx64` for future ln-map precision layering.

## Goal

The target optimization is a layered ln-map renderer:

- shallow rows use faster low-precision kernels when the visual error is
  acceptable;
- deeper rows upgrade to higher precision;
- CUDA should avoid `fp64` on consumer GPUs where double precision throughput is
  poor;
- CPU paths should use SIMD `fp32` only when the measured speed/quality tradeoff
  is favorable.

The key point for ln-map is that a pixel is not usually an exactly shared
complex number across scalar types. Each pixel maps from `(row, col)` through
`theta`, `exp(k)`, `cos(theta)`, and `sin(theta)`, so a native `fp32` ln-map
kernel generates a different `c` than native `fp64`.

## Hardware

Measured locally on:

- CPU: AMD Ryzen 9 7945HX with AVX2, FMA, and AVX512F.
- GPU: NVIDIA GeForce RTX 4070 Laptop GPU, compute capability 8.9, 8 GiB VRAM.
- CUDA toolkit: 12.6.

## Experiment Setup

Mandelbrot escape iteration was used as the baseline because the current ln-map
renderer's hot path is `z -> z^2 + c` escape iteration.

Common parameters:

- `max_iter = 1024`
- bailout squared: `4.0`
- ln-map center: `-0.7436438870371587 + 0.13182590420531198i`

Two error models were tested:

- `same-c`: the same mathematical complex coordinate is converted to each
  scalar type before iteration.
- `native ln-coordinate`: each scalar path generates its own ln-map coordinate
  from `(row, col)`, matching how a real ln-map kernel behaves.

## Same-c Error

Random uniform points in the original domain:

```text
points=50000 max_iter=1024
fp32-vs-fp64 mismatch=129/50000 (0.258%) mean_abs_iter=0.2294 p99=0 max=636
fx64-vs-fp64 mismatch=3/50000   (0.006%) mean_abs_iter=0.0026 p99=0 max=83
```

Boundary-biased points near Mandelbrot cardioid/bulb boundaries:

```text
points=50000 max_iter=1024
fp32-vs-fp64 mismatch=3065/50000 (6.130%) mean_abs_iter=7.7203 p99=285 max=862
fx64-vs-fp64 mismatch=78/50000   (0.156%) mean_abs_iter=0.1193 p99=0   max=466
```

Interpretation:

- `fx64` stays close to `fp64` for the same input coordinate.
- `fp32` is acceptable away from boundaries, but boundary points amplify small
  numeric differences into large iteration differences.

## Native ln-coordinate Error

Full-column ln-map sample:

```text
width_s=1024 height_t=2048 sample_step=1 points=2097152 max_iter=1024
coord fp32-vs-fp64 mean_abs_c=5.067e-08 p95=2.124e-07 p99=5.787e-07 max=1.705e-06
coord fx64-vs-fp64 mean_abs_c=5.020e-20 p95=0.000e+00 p99=3.469e-18 max=8.163e-18

native fp32-vs-fp64 mismatch=222560/2097152 (10.612%) mean_abs_iter=8.0257 p95=28 p99=234 p999=558 max=930
native fx64-vs-fp64 mismatch=3366/2097152   (0.161%)  mean_abs_iter=0.0940 p95=0  p99=0   p999=4   max=594
```

Depth buckets:

```text
row    0- 255 depth~0.00 oct coord32_mean=2.34e-07 fp32_mis=0.12% max=647 fx64_mis=0.00% max=1
row  256- 511 depth~2.27 oct coord32_mean=6.04e-08 fp32_mis=0.75% max=819 fx64_mis=0.01% max=316
row  512- 767 depth~4.53 oct coord32_mean=2.35e-08 fp32_mis=1.61% max=862 fx64_mis=0.04% max=382
row  768-1023 depth~6.80 oct coord32_mean=1.80e-08 fp32_mis=4.23% max=888 fx64_mis=0.07% max=442
row 1024-1279 depth~9.06 oct coord32_mean=1.75e-08 fp32_mis=14.81% max=885 fx64_mis=0.32% max=513
row 1280-1535 depth~11.33 oct coord32_mean=1.74e-08 fp32_mis=8.84% max=872 fx64_mis=0.12% max=480
row 1536-1791 depth~13.60 oct coord32_mean=1.74e-08 fp32_mis=24.64% max=875 fx64_mis=0.33% max=594
row 1792-2047 depth~15.86 oct coord32_mean=1.74e-08 fp32_mis=29.90% max=930 fx64_mis=0.40% max=585
```

Interpretation:

- `fp32` coordinate error is often only `1e-8` to `1e-6`, but near fractal
  boundaries that is enough to change many escape iterations.
- The error profile is not monotonic only by depth; it also depends on how the
  strip crosses boundary structure.
- `fx64` native coordinates are effectively identical to `fp64` at this scale.

## CUDA Speed

Native ln-coordinate CUDA microbenchmark:

```text
pixels=131072 width_s=512 height_t=2048 sample_step=8 max_iter=1024
fp64 kernel_ms=74.469 actual_iters=76400112 Miter/s=1025.93
fp32 kernel_ms=0.368  actual_iters=75507360 Miter/s=205218.74
fx64 kernel_ms=2.616  actual_iters=76401077 Miter/s=29210.66
```

Approximate relative throughput on the measured RTX 4070 Laptop GPU:

```text
CUDA fp32 ~= 202x fp64
CUDA fx64 ~= 28.5x fp64
CUDA fp32 ~= 7x fx64
```

Interpretation:

- CUDA `fp64` should not be a primary ln-map path on this class of GPU.
- CUDA `fp32` is extremely fast but needs row/region validation.
- CUDA `fx64` is much slower than `fp32` but still far faster than CUDA `fp64`
  and much more accurate than `fp32`.

## CPU SIMD Speed

Native ln-coordinate CPU SIMD microbenchmark:

```text
width_s=1024 height_t=2048 sample_step=1 max_iter=1024 omp_threads=32
scalar-fp64   ms=56.826 actual_iters=633742920 Miter/s=11152.33
scalar-fp32   ms=48.309 actual_iters=633668490 Miter/s=13116.96
avx2-fp32     ms=18.936 actual_iters=633668490 Miter/s=33463.54
avx512-fp32   ms=7.065  actual_iters=633668490 Miter/s=89693.55
```

All `fp32` CPU paths produced the same iteration results:

```text
fp32-vs-fp64 mismatch=222560/2097152 (10.612%) mean_abs_iter=8.0257 p95=28 p99=234 p999=558 max=930
```

Interpretation:

- Scalar `fp32` is not a useful proxy for SIMD `fp32`.
- On Ryzen 9 7945HX, continuous-column `AVX512 fp32` is substantially faster
  than `AVX2 fp32` and scalar `fp64`.
- `AVX512 fp32` is worth implementing for shallow ln-map rows, subject to
  quality validation.

## Design Implications

Recommended future ln-map planner:

```text
if CUDA available:
  shallow rows: cuda_fp32
  middle rows:  avx512_fp32 or avx512_fp64 after validation
  deep rows:    cuda_fx64
else:
  shallow rows: avx512_fp32 if validation passes
  middle/deep:  avx512_fp64
  extreme:      fx64 only when fp64 depth is insufficient
```

Avoid fixed depth thresholds as the only policy. A better planner should:

- split rows into depth bands;
- sample representative pixels in each band;
- compare candidate precision against a reference precision;
- promote a band when mismatch or color/iteration error exceeds a threshold;
- record the selected backend/scalar per band for progress reporting and debug.

Initial conservative thresholds could be:

```text
fp32 acceptable when:
  sampled mismatch ratio <= 1%
  and p99 abs iteration delta <= 16

otherwise promote to fp64 or fx64.
```

These thresholds should be treated as render-quality policy, not numeric truth.
The final UI may expose presets such as `fast`, `balanced`, and `strict`.

## Recommended Implementation Plan

1. Add `scalar_type` to `LnMapParams` and API requests.
2. Add `ln_map_avx512_fp32` and `ln_map_avx2_fp32` for the first 10 quadratic
   variants.
3. Add CUDA `ln_map_fp32` and CUDA `ln_map_fx64`.
4. Add a row-band planner that can assign:
   - `cuda_fp32`
   - `avx512_fp32`
   - `avx512_fp64`
   - `avx2_fp64`
   - `cuda_fx64`
5. Add validation sampling before full render, with results written into run
   progress/report JSON.
6. Preserve current `fp64` ln-map as the strict reference path.


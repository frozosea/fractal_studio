# Feature Status / 功能状态

本文记录已经确认的功能状态和暂缓决策，避免把已完成能力重复列为待办，也避免在语义与生命周期约定尚未确定时提前固化接口。

决策日期：2026-07-17。

## Decision Summary / 决策摘要

| Feature / 功能 | Status / 状态 | Decision / 决策 |
|---|---|---|
| AVX2 `fp32` raw field | Implemented / 已实现 | 当前实现已覆盖 AVX2 `fp32` raw-field 路径，不再作为待办。 |
| Transition map rotation | Fixed / 已修复 | 前端预览和 OpenMP/AVX2/CUDA pair/multi viewport rotation 已统一，并有差分回归覆盖。 |
| Bird/Celtic Ship transition fold | Fixed / 已修复 | pair/volume 的 OpenMP、AVX2、CUDA 统一为 `2*abs(x*axis)`；旋转 pair/multi 与 raw-volume 硬件路径均有回归。 |
| Non-cardinal transition fp80/fp128 | Implemented / 已实现 | 双变体 viewport 与完整 3D orbit 使用所请求高精度标量；multi/pairwise fallback 继续报告实际 fp64。 |
| Single-strip ln-map reuse guard | Implemented / 已实现 | preview stats 和非分段 full-strip 复用校验完整生成身份；缺少持久全局 CDF 的 `bands`/`frontier` 暂不复用。 |
| Artifact nested paths and streaming | Implemented / 已实现 | artifact ID 保存 run-relative path；download/content 支持流式发送和单 Range，不再整文件读入内存。 |
| Multi-variant transition video | Deferred / 暂缓 | multi-kernel `theta` 尚无明确语义；先定义权重与路径动画、schema 和 tests。 |
| Segmented ln-map reuse | Deferred / 暂缓 | 临时段默认清理，且尚无持久 manifest、兼容性约定与保留策略；现有 preview stats reuse 继续。 |

## AVX2 `fp32` Raw Field

状态：已实现。

AVX2 `fp32` raw-field 计算路径已经落地，因此不再将它列为缺失功能。该结论只覆盖 `fp32` raw field，不表示其他 scalar、metric 或 variant 自动具备相同的 AVX2 支持范围。

相关实现位于 [`backend/src/compute/map_kernel_avx2.cpp`](../backend/src/compute/map_kernel_avx2.cpp)。

## Transition Map Rotation

状态：已修复。

Transition 探索视图的非零 `rotationDeg` 已在 OpenMP、AVX2、CUDA 的 pair/multi 与 escape/metric 路径统一；前端旧帧预览也按最短角和已渲染坐标系组合旋转/平移。负 cardinal slice 的直接 2D 快捷路径同步镜像 center、Julia imaginary 和 rotation，避免切到 `-90°/±180°` 时错位。

## Multi-variant Transition Video

状态：暂缓。

当前没有为多个 kernel 定义统一的 `theta` 语义。开始实现视频导出前，需要先确定：

1. 各 variant 权重如何随时间变化，包括归一化、边界和插值规则。
2. transition 路径如何动画化，包括路径顺序、时间分配以及权重动画与路径动画的组合方式。
3. API 与持久化元数据所使用的 schema，以及向后兼容规则。
4. 覆盖 schema 校验、动画语义、确定性输出和现有单段 transition 行为的 tests。

在这些约定完成前，保留现有单段 transition 视频能力，不把实时预览或 PNG 中的多变体参数直接解释成视频时间轴。
前端继续禁用多变体视频入口；后端收到 `transitionMode="multi"`、`transitionVariants` 或 `transitionWeights` 时明确返回 `400`，不会静默退化成双变体视频。

## Segmented ln-map Reuse

状态：暂缓。

分段 ln-map 的临时段目前默认清理，尚未形成可供后续任务安全复用的持久化契约。恢复该功能前，需要补齐：

1. 持久 manifest，用于描述完整段集合及复用所需元数据。
2. manifest、分段格式和生成参数的兼容性规则与校验。
3. 分段产物的保留、清理和失效策略。

现有 preview stats reuse 继续保留；该优化不等同于承诺复用已清理的分段 ln-map 产物。
当当前导出计划需要分段且请求同时携带 `lnMapRunId` 时，后端明确返回 `400`。

# Known Issues / 问题清单

盘点日期：2026-07-27。基线：`master@222bbb1`。
修复批次一（2026-07-27）：问题 3/4/5/8/9/11 已改，逐条结论写在各自章节末尾。

本文只记录**当前代码里可验证的问题**，不记录路线图愿望。每条给出证据位置、影响和建议。
与 [feature_status.md](feature_status.md) 的分工：那里记录**已决策**的实现/暂缓结论，这里记录**尚未决策或已偏离决策**的缺口。

## 优先级摘要

| # | 问题 | 层 | 严重度 | 状态 |
|---|---|---|---|---|
| 1 | Compute 18 个 kind 只接入 4 个 | Platform | 高 | 待决策 |
| 2 | 后端已支持的 video / hs_mesh / transition_mesh 在前端无入口 | 前端 | 高 | 待办 |
| 3 | `zoom_video` 的 `depthOctaves` 硬编码 20.0 | Platform | 高 | **已修** |
| 4 | 前端预览节流(420ms)超出后端限流(30/min) | 前后端 | 高 | **已修** |
| 5 | `colorProgram` 可能随 `zoom_video` 下传，违反染色合同 | Platform | 中 | **已修** |
| 6 | capabilities 只投影 `map_image` 一个 job | Platform | 中 | 待办 |
| 7 | hs_mesh/transition_mesh 双产物被丢弃一个 | Platform | 中 | 待决策 |
| 8 | `poll_run_status` 在非事务连接上取行锁 | Platform | 中 | **已修** |
| 9 | `_map_2d` 直接下标取键，规范化漂移即 500 | Platform | 中 | **已修** |
| 10 | 前端 `fallbackCapabilities` 硬编码，与后端能力漂移 | 前端 | 低 | 待办（依赖 6） |
| 11 | 前端缩放滑块量程与实际 scale 下限不匹配 | 前端 | 低 | **已修** |
| 12 | 进度文档停留在 2026-07-24，严重落后于代码 | 文档 | 中 | 待办 |

---

## 1. Compute 18 个 kind 只接入 4 个

证据：[compute_request_mapper.py:78-121](../platform-backend/app/studio/compute_request_mapper.py#L78-L121)。

`map_durable_v1` 只映射 `image→map_image`、`video→zoom_video`、`hs_mesh`、`transition_mesh`，其余走 `raise ValueError("unsupported_output_kind")`。预览侧 [:66-71](../platform-backend/app/studio/compute_request_mapper.py#L66-L71) 只有 `map_image`。

未接入的 14 个（对照 [compute_v1_jobs.md](compute_v1_jobs.md)）：

`raw_field`、`ln_map`、`video_preview`、`legacy_zoom_video`、`transition_image`、`transition_video`、`transition_video_preview`、`hs_field`、`transition_voxels`、`special_points_enumerate`、`special_points_search`、`special_points_snap`、`special_points_auto`、`special_points_seed`、`benchmark`。

影响：C++ 侧 C1/C2 已标记 100% 完成并有完整合同文档，但这些能力对浏览器完全不可达。README 宣传的 3D、视频、特殊点、系统诊断在 Platform 产品线上不存在。

建议：逐个决策——排期接入，或在 [feature_status.md](feature_status.md) 里显式标注「Platform 暂缓」。**不要让它们继续悬空**：现在既不在待办里，也不在暂缓决策里。

## 2. 后端已支持的三种 output 在前端无入口

证据：[studio/page.tsx](../frontend/src/app/[locale]/(workbench)/studio/page.tsx) 的 `saveAndRender` 只构造 `{ kind: "image", format: "png" }`。

后端 [studio/models.py:114-177](../platform-backend/app/studio/models.py#L114-L177) 已定义 `ImageOutputSpec`、`VideoOutputSpec`、`HsMeshOutputSpec`、`TransitionMeshOutputSpec` 四个判别联合分支，mapper、worker `_select_artifacts`、`assets/service.py:166` 的 media type 映射全部就绪。

影响：video / hs_mesh / transition_mesh 三条链路的后端代码和测试是死代码。这是当前**投入产出比最高**的缺口——纯前端工作。

建议：优先补 UI。同时把 116 行的单文件 studio 页面拆分（现在是超长单行风格，`frontend/src/components/studio/` 下只有一个 73 行 canvas 组件），否则加三个输出面板后难以维护。

## 3. `zoom_video` 的 `depthOctaves` 硬编码

证据：[compute_request_mapper.py:88-93](../platform-backend/app/studio/compute_request_mapper.py#L88-L93)

```python
"depthOctaves": 20.0,
# Product MVP controls duration, while Compute requires a zoom path too.
```

影响：用户能选 fps 和时长，但决定视频「变焦多深」的核心数学参数不可控，所有导出视频的缩放路径完全相同。注释自己承认这是 MVP 占位。

建议：提升为 `VideoOutputSpec` 字段，带合理默认值与范围校验（参考 `compute_v1_jobs.md` 的 `zoom_video` 限制）。

**已修**：`VideoOutputSpec.depth_octaves`（别名 `depthOctaves`，默认 20.0，范围 0.05..1024，与 Compute 一致），
mapper 改读 output spec，历史 job（spec 里没有该键）回落到 20.0。前端尚未暴露该控件——见问题 2。

## 4. 前端预览节流超出后端限流

证据：

- 后端：`preview_rate_limit_per_minute` 默认 **30**（[config.py:44](../platform-backend/app/core/config.py#L44)），即平均 1 次 / 2 秒；超限返回 `429 preview_rate_limited`。
- 前端：预览 effect 的 debounce 是 **420ms**（studio page），且依赖 `specKey`——任何 spec 变化都触发。

影响：拖动「Zoom depth」滑块、连续调色、快速改迭代数都会在数秒内打满配额，用户看到红色报错条。这不是理论风险，是正常交互路径。

建议：三选一或组合——前端 debounce 提到 ≥1.5s 并对进行中请求做合并；前端本地令牌桶；后端对同一 canonical spec hash 的重复预览不计数。另外 `429` 应该在 UI 上降级为「稍候」提示而非错误。

**已修**：debounce 降为 600ms，但增加 `previewMinIntervalMs = 2100` 的客户端节流（下一次请求排到上一次
发出后 2.1s），平均速率稳定在 ~28/min；命中 `429` 时不再显示红色错误条，而是琥珀色「稍候」提示并把节流
窗口再推后一轮后自动重试（`retryTick`）。后端限流本身未改。

## 5. `colorProgram` 可能随 `zoom_video` 下传

证据：[compute_request_mapper.py:49-51](../platform-backend/app/studio/compute_request_mapper.py#L49-L51) 的 `_map_2d` 会透传 `colorMap`/`colorProgram`；[:86](../platform-backend/app/studio/compute_request_mapper.py#L86) 的 `video` 分支直接复用 `_map_2d`。

[coloring_contract.md](coloring_contract.md) 与 commercialization 记录约定：自定义 gradient **首批只在二维 preview/PNG 启用，其他输出必须显式拒绝**。

影响：若 Compute 侧未拒绝，则违反合同静默生效；若拒绝，则用户开着自定义渐变点「导出视频」会拿到一个来自 Compute 的 `422`，Platform 层没有更早、更友好的拦截。

建议：在 mapper 的 `video` 分支显式剔除或拒绝 `colorProgram`，并补一条单元测试。需要先确认 Compute 侧实际行为——两种结果都要求 Platform 改动。

**已修**：选择「拒绝」而非静默剔除——静默剔除会让用户拿到一个和预览不一样的视频。`video` 分支带
`colorProgram` 时抛 `color_program_unsupported_for_output`，`render_job_service` 把它作为 422 detail 原样返回
（新增 `PUBLIC_MAPPING_ERRORS` 白名单，其余映射异常仍是笼统的 `unsupported_render_output`）。
单元测试见 `tests/unit/test_render_mapper.py`。

## 6. capabilities 只投影 `map_image`

证据：[capability_service.py](../platform-backend/app/studio/capability_service.py) 只从 `jobs[]` 里 `next(... kind == "map_image")`，取它的 metrics/engines/scalars。

影响：mesh / video 的可用 engine、scalar、分辨率上限全部对前端不可见。前端只能靠硬编码猜（见问题 10）。一旦接入问题 1 里的新 kind，这个投影必然要重写。

建议：改成按 kind 分组返回完整能力矩阵，前端按当前输出类型取对应子集。

## 7. 双产物被丢弃一个

证据：[render_worker.py `_select_artifacts`](../platform-backend/app/studio/render_worker.py) 按 `output_spec["format"]` 选 MIME，然后 `[:1]`。

[compute_v1_jobs.md:289](compute_v1_jobs.md) 规定 `hs_mesh` 必需产物是 `hs_mesh.glb` **与** `hs_mesh.stl` 两个。

影响：用户选 glb 就永久丢失同一次计算已经产出的 stl，想要另一格式必须重新付费计算一遍。

建议：确认这是产品决策还是遗漏。若是决策，写进 feature_status；若是遗漏，改为摄取全部必需产物、由资产层暴露多格式下载。

## 8. `poll_run_status` 在非事务连接上取行锁

证据：[render_worker.py](../platform-backend/app/studio/render_worker.py) 的 `poll_run_status` 和 `forward_cancellation` 用 `get_engine().connect()`（其余路径用 `.begin()`）调用 `lock_for_worker`。

影响：`lock_for_worker` 的 `FOR UPDATE` 语义在这里形同虚设——锁随连接关闭立即释放，读到的状态在后续长耗时的 Compute HTTP 调用期间可能已被改写。目前靠后续 `save_compute_success` / `cancel_and_release` 的乐观状态检查兜底，所以不一定表现为线上 bug，但**读到的 `job.status` 与之后的写入之间存在真实竞态窗口**。

建议：要么改用 `.begin()` 并缩短临界区，要么明确这里就是无锁快照读、把函数改名并去掉 `FOR UPDATE`，避免误导后续维护者。

**已修**：取后者。新增 `render_job_repository.read_snapshot()`（同一查询，无 `FOR UPDATE`），
`poll_run_status` / `forward_cancellation` 改用它。竞态窗口本身仍在，但由后续写入的乐观状态检查兜底，
代码不再假装持锁。`lock_for_worker` 仍只在 `.begin()` 路径上使用。

## 9. `_map_2d` 直接下标取键

证据：[compute_request_mapper.py:32-48](../platform-backend/app/studio/compute_request_mapper.py#L32-L48) 对 `iterations`/`variant`/`centerRe`/… 全部用 `canonical_spec["..."]`。

影响：canonicalization 若少写一个键，抛 `KeyError` 而非结构化错误。在预览路径上会变成 500 而不是 422。可选键走 `if optional in` 的宽松分支，必需键却没有对称保护。

建议：`_map_2d` 顶部做一次必需键校验，缺失时抛 `ValueError("incomplete_canonical_spec")`，与既有 `unsupported_recipe_version` 风格一致。

**已修**：按建议实现（`_REQUIRED_2D_KEYS`）。该 code 也在 `PUBLIC_MAPPING_ERRORS` 里，durable 路径返回 422。
mesh 分支仍直接下标取 `centerRe/centerIm/scale/...`，键集是 2D 的子集，暂未单独加校验。

## 10. 前端 `fallbackCapabilities` 硬编码

证据：studio page 顶部的 `fallbackCapabilities` 写死了 metrics、engines（只有 `auto`/`openmp`）、scalars、11 个 colorMaps。

影响：`/studio/capabilities` 返回 503 时（Compute 不可用），UI 会展示一份可能过期的能力表，用户选了实际不支持的组合，直到提交才在 Compute 侧报 422。engines 列表尤其可疑——后端 Literal 允许 `cuda`/`avx2`/`avx512`/`hybrid`。

建议：Compute 不可用时禁用相关控件而非用假数据填充；或把 fallback 缩到最小安全集。

## 11. 缩放滑块量程不匹配

证据：`zoomLevel = clamp(round(log2(3 / scale)), 0, 36)`，而 `zoom()` 允许 scale 低至 `1e-12`。

`log2(3 / 1e-12) ≈ 41.4`，被截到 36。

影响：用滚轮/双击深入到极限后，滑块停在 36 不再反映真实深度，再拖动滑块会把视图**跳回**到较浅的位置，丢失用户辛苦找到的坐标。

建议：把滑块上限与 scale 下限对齐（≈42），或反过来把 `zoom()` 的 scale 下限收到 `3 / 2**36`。

**已修**：引入 `zoomMaxLevel = 41` 与 `minScale = 3 / 2 ** 41`（≈1.36e-12，仍在原 `1e-12` 下限之内），
滑块 `max` 与 `zoom()` 的 clamp 共用同一常量，两者不再各说各话。

## 12. 进度文档严重落后

证据：[commercialization_implementation.md](commercialization_implementation.md) 标注「最后更新 2026-07-24」，进度快照写着：

- 前端双轨 F0：**0%**，「尚未开始拆分前端 API 与页面」
- 商业模块 M1–M6：**0%**，「身份、资产、市场、支付和账本尚未开始」
- 完整商业化路线：**约 30%**

实际代码里 auth/CSRF、recipes、render-jobs、assets + 下载 URL、marketplace（explore/listings/favorites）、checkout + 支付宝 webhook、purchases、payouts（含 operator 人工打款）全部落地，`platform-backend/tests/e2e/` 下有 20 个端到端测试文件；前端 `(workbench)` 下 8 个页面齐备。

影响：该文档自称「执行依据与进度事实来源」，当前状态会让任何依据它做排期的人得出错误结论。

建议：重写进度快照与 M1–M6 勾选项。**这件事应该和上面任何一条代码改动同批完成**，否则问题清单本身也会很快过期。

---

## 附：核对方法

- kind 覆盖：`grep -oE '"(map_image|raw_field|ln_map|...)"' platform-backend/app` 对照 `docs/compute_v1_jobs.md` 的 18 个章节。
- 路由覆盖：`grep -rn "@router\." platform-backend/app/*/router.py`。
- 前端调用面：`frontend/src/lib/api/platform.ts`。

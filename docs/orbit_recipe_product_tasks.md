# Orbit 编排、Repeat Block 与配方存档任务清单

本文定义 Orbit 组合玩法从编辑器到商业存档、再到 Compute v1 的产品边界。服务后端和前端可以据此并行实施；C++ Compute 仍只负责数学执行，不保存用户配方、草稿或作品历史。

## 1. 产品语义

用户可以把内置公式和安全 DSL 公式组成确定性时间表：

```text
[Mandelbrot ×2] [Burning Ship ×3] [Custom A ×1]
```

`×N` 表示该公式连续执行 N 次。时间表结束后从头循环，直到渲染请求的 `iterations`。每个像素使用相同日程，DSL 中的 `n` 始终是从 0 开始的全局迭代序号。

编辑器增加 `repeat_block`，用于表达“把一组步骤重复若干次”：

```json
{
  "type": "repeat_block",
  "count": 3,
  "steps": [
    {"type": "formula_step", "formulaRef": "mandelbrot", "span": 2},
    {"type": "formula_step", "formulaRef": "burning_ship", "span": 1}
  ]
}
```

其展开结果为：

```text
M M B | M M B | M M B
```

规则：

- `count` 和 `span` 都是正整数；空 block 非法。
- block 可以嵌套，但 Platform 编译器必须限制深度、节点数和展开后的周期长度。
- `repeat_block` 只重复日程，不重置 `z`、`c` 或全局 `n`，也不创建新的轨道。
- 顶层时间表仍循环到 `iterations`；block 的 `count` 是每个顶层周期内的有限重复次数。
- 随机选择、条件分支、weighted schedule 和 output blend 不是 repeat block，必须使用未来独立节点。

## 2. 协议边界和编译流程

`repeat_block` 首期是 Platform 配方编辑 IR，不直接发送给当前 Compute v1。原因是现有递归 `sequence` 使用全局 `n` 取内层周期相位，不能可靠表达用户直觉中的局部重复块。

```text
OrbitEditorDocument
  -> validate references and limits
  -> expand repeat_block
  -> merge adjacent identical formula steps
  -> remove UI-only ids/names/folding state
  -> emit flat Compute v1 sequence
  -> canonical JSON + platform recipe hash
```

例如上面的 block 编译为：

```json
{
  "type": "sequence",
  "repeat": true,
  "steps": [
    {"span": 2, "program": {"type": "formula", "formula": {"type": "builtin", "id": "mandelbrot"}}},
    {"span": 1, "program": {"type": "formula", "formula": {"type": "builtin", "id": "burning_ship"}}},
    {"span": 2, "program": {"type": "formula", "formula": {"type": "builtin", "id": "mandelbrot"}}},
    {"span": 1, "program": {"type": "formula", "formula": {"type": "builtin", "id": "burning_ship"}}},
    {"span": 2, "program": {"type": "formula", "formula": {"type": "builtin", "id": "mandelbrot"}}},
    {"span": 1, "program": {"type": "formula", "formula": {"type": "builtin", "id": "burning_ship"}}}
  ]
}
```

编译器应在展开后合并相邻且规范化公式 hash 相同的步骤。提交 Compute 前必须执行 Compute v1 的 64 steps、1,000,000 cycle span、256 nodes、32 depth 和 DSL 资源限制；超限时返回可定位到编辑器节点的产品错误，不能截断或静默简化。

这些上限由 `orbit_program.cpp`（`MAX_SEQUENCE_STEPS=64`、span 1..1,000,000、`MAX_AST_NODES=256`、`MAX_AST_DEPTH=32`）在 Compute v1 运行时强制执行，与展开逻辑跑在 Platform 还是 Compute 无关：合法的扁平 sequence 本身就被卡在 64 个短 step 以内，因此请求体大小不是引入原生 `repeat_block` IR 的理由；Platform 编译器只需在调用 Compute 前做同等校验并拒绝超限文档。

若未来需要保存而不展开数千个 block、逐 block 进度或局部迭代相位，再为 Compute 新增明确的 `repeat_block` IR 节点和 capability flag。该升级必须规定 `n` 是否重置；默认仍建议不重置，并以 golden parity 证明和扁平展开语义一致。

## 3. 配方与“奇形怪状”存档

存档是 Platform 产品事实，不写入 Compute SQLite。推荐对象：

- `Recipe`：用户可见的逻辑作品，保存标题、说明、所有者和当前草稿指针。
- `RecipeRevision`：不可变版本，保存完整编辑文档、编译后的 Compute payload、两个规范化 hash、capability snapshot 和逃逸分析快照。
- `RecipeDraft`：可变自动保存内容，使用乐观版本号防止多标签页覆盖。
- `RenderJob`：引用确定的 revision，不引用会继续变化的 draft。
- `SavedView`：可选的命名视口/存档点，保存 center、scale、rotation、Julia 参数、配色和预览引用；它不改变公式本身。

建议用户动作：

- “保存草稿”：覆盖自己的 draft，并保留最近自动保存恢复点。
- “创建版本”：生成不可变 revision，可用于渲染、分享、商品发布和复现。
- “另存为”：复制为新的 Recipe，记录来源 revision，但所有权独立。
- “保存视角”：在同一 revision 下记录某个有趣位置，适合保存偶然发现的奇形怪状。
- “从任务恢复”：从 RenderJob 的不可变快照创建新草稿，不能反向修改历史任务。

任何市场发布版本、订单快照或已完成 RenderJob 都必须引用不可变 revision。删除 Recipe 时不得级联删除已售商品、订单事实或商业 Asset。

## 4. 服务后端任务清单

### 4.1 数据与版本

- [ ] 按商业 PDF 的 UUID、响应包络、会话和权限规则建立 Recipe、Draft、Revision、SavedView。
- [ ] Revision 同时保存 `editor_document`, `compiled_compute_payload`, `editor_hash`, `platform_recipe_hash`, `schema_version`。
- [ ] 保存创建时的 Compute capability snapshot、DSL/Orbit 编译结果和 escape analysis；渲染完成后另存实际 manifest 事实。
- [ ] Draft 使用 `version`/ETag 或等价 compare-and-swap；冲突返回明确的 `409`，不得最后写入者静默覆盖。
- [ ] 所有权、只读分享、协作者和管理员权限接入 M1 RBAC；禁止通过 recipe ID 越权读取 DSL 或私有参数。

### 4.2 校验与编译

- [ ] 为编辑 IR 建立 Pydantic discriminated union：`formula_step`, `repeat_block`；公式为 builtin 或 DSL definition/reference。
- [ ] 服务端作为权威编译器展开 block、解析引用、合并相邻步骤并生成扁平 Compute v1 Orbit Program。
- [ ] 限制原始文档大小、嵌套深度、节点数、公式数、展开步数、周期长度和 DSL 参数；在调用 Compute 前失败。
- [ ] 错误包含稳定 code 和编辑器 `nodeId`/JSON pointer，不返回 Python/C++ 堆栈。
- [ ] 对规范化编辑文档和编译 payload 分别计算 hash；重复编译必须跨进程、跨重启一致。
- [ ] 编译结果必须再次与节点 capabilities 校验；不支持的 job kind/metric/engine 不降级。

### 4.3 API 与任务

- [ ] 实现草稿读取/保存、创建 revision、版本列表/详情、另存为和 SavedView CRUD。
- [ ] preview/render API 只接收 draft/revision + override allowlist，不提供任意 Compute JSON 代理。
- [ ] 创建 RenderJob 时在同一事务固化 revision/请求快照、配额 reservation 和 Outbox event。
- [ ] 返回前端可显示的编译摘要：周期长度、展开步数、公式数、DSL 状态、escape certificate、预计执行路径。
- [ ] 为 schema migration 保留 `schemaVersion`；旧版本读取时迁移到内存，新 revision 始终写最新版本，历史原文不可变。

### 4.4 服务后端验收

- [ ] repeat block 展开与手写扁平 sequence 的 hash/渲染结果一致。
- [ ] DSL 可以出现在任意 formula step；引用缺失、递归引用和资源超限有独立测试。
- [ ] 自动保存冲突、越权访问、版本不可变、另存为来源和删除保留商业事实均有测试。
- [ ] RenderJob 可仅凭 revision 和 manifest 跨重启复现请求与结果。

## 5. 前端任务清单

### 5.1 编辑器模型与组件

- [ ] 建立与 Platform 编辑 IR 对应的 TypeScript discriminated union，不直接复用 Compute wire DTO。
- [ ] Orbit 时间线支持添加、删除、复制、拖拽排序公式步骤，编辑 `span`。
- [ ] 支持选中多个步骤创建 repeat block、设置 `count`、解组、折叠和嵌套缩进。
- [ ] formula picker 同时提供内置公式、用户 DSL 公式和“新建自定义公式”。
- [ ] DSL 编辑器显示语法位置、类型错误、参数类型/值和危险资源限制；错误应定位到对应 block/step。
- [ ] 显示编译后的周期预览，例如 `M×2 B×1` 重复三次；长周期采用摘要，不渲染百万个格子。

### 5.2 保存与恢复体验

- [ ] 明确区分保存草稿、创建版本、另存为、保存视角和提交渲染。
- [ ] 自动保存采用 debounce，并携带 draft version；409 时提供比较/重新加载，不直接覆盖。
- [ ] 配方页展示版本时间线、预览图、formula/period 摘要和从历史版本创建草稿。
- [ ] SavedView 支持在 MapCanvas 当前视口一键保存、命名、恢复和删除。
- [ ] 离开未保存页面、DSL 编译中和版本创建失败时提供明确状态。

### 5.3 能力与数学提示

- [ ] 从 Platform 获取节点能力摘要；禁用不兼容产物，而不是提交后再静默替换。
- [ ] 展示 `certified_finite`、`unverified` 等逃逸状态；含任意 DSL 的无证书结果称为“有限迭代轨道图”。
- [ ] 展示实际硬件结果和 fallback，不用用户选择的 engine 冒充实际执行硬件。
- [ ] repeat block 文案明确“不重置 z/c/n”；提供展开预览帮助理解。

### 5.4 前端验收

- [ ] 编辑、拖拽、分组、嵌套、解组后生成的文档稳定且不丢 nodeId。
- [ ] 桌面、平板和移动端时间线可操作，保留现有 tokens、NavRail、StatusRail 和画布风格。
- [ ] 超长时间表使用虚拟化/摘要，交互不会因展开数量冻结。
- [ ] API contract、自动保存冲突、历史恢复和关键布局有自动测试与视觉回归。

## 6. 明确不在本批次实现

- Compute v1 原生 `repeat_block` 节点。
- 随机/条件日程、weighted schedule、output blend 和参数动画曲线。
- 用户公式市场、多人实时协同编辑和公开 fork 社交图谱。
- 为任意 DSL/组合自动生成 SIMD/CUDA kernel。

这些能力可以复用同一版本化编辑文档和不可变 Revision，但必须分别设计数学语义、capability 和迁移规则。

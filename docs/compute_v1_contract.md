# Compute v1 Private HTTP Contract / 私有计算合同

Compute v1 是 FastAPI Platform Worker 调用的私有协议，不是浏览器公共 API。当前 schema version 为 `1`；服务端不支持的 kind、Orbit 节点或数学/输出组合必须显式失败，不得替换为 Mandelbrot。

## Authentication and endpoints

除存活探针外，请求必须包含：

```http
Authorization: Bearer <FSD_COMPUTE_SERVICE_KEY>
```

| Method | Path | Success | Purpose |
|---|---|---:|---|
| `GET` | `/compute/v1/health` | `200` | 无鉴权存活探针。 |
| `GET` | `/compute/v1/capabilities` | `200` | kind、Orbit 兼容矩阵和运行时硬件快照。 |
| `POST` | `/compute/v1/previews` | `200` | 同步短预览；`map_image` 返回二进制 RGBA，其余返回版本化 JSON。 |
| `POST` | `/compute/v1/runs` | `202` | 创建或按幂等键复用持久 run。 |
| `GET` | `/compute/v1/runs/{id}` | `200` | 查询状态、进度、产物摘要和硬件证据。 |
| `POST` | `/compute/v1/runs/{id}/cancel` | `202` | 请求协作取消；终态以随后轮询结果为准。 |
| `GET` | `/compute/v1/runs/{id}/manifest` | `200` | 获取完整产物校验、配方 hash、证明和硬件证据。 |
| `GET` | `/compute/v1/artifacts?artifactId=...` | `200/206` | 私有流式读取，支持单一 `Range`。 |

受保护路由缺少或使用错误密钥时返回 `401 COMPUTE_UNAUTHORIZED`。服务密钥为空不会降级为匿名访问。

## Request envelope

预览和 run 共用判别包络；持久 run 额外要求 1–200 字符的稳定幂等键：

```json
{
  "schemaVersion": 1,
  "kind": "map_image",
  "idempotencyKey": "platform-job:018f...",
  "payload": {
    "centerRe": -0.75,
    "centerIm": 0.0,
    "scale": 3.0,
    "width": 1024,
    "height": 1024,
    "iterations": 1000,
    "engine": "auto",
    "scalarType": "auto"
  }
}
```

相同 `idempotencyKey` 返回第一次创建的同一个 `computeRunId`。调用方不得为重试生成新 key。

## Capability matrix

`GET /compute/v1/capabilities` 的 `jobs[]` 来自服务端单一注册表。每项包含 `kind`、`persistent`、`preview`、`orbitProgram`、`variantProfile`、`metrics`、`engines`、`scalars` 和 `outputMediaTypes`；兼容性校验和顶层 kind 清单使用同一注册表。Platform 应读取这些字段，不要在客户端复制一份可能过期的矩阵。

持久 kind：

- `map_image`, `ln_map`, `zoom_video`, `legacy_zoom_video`, `transition_video`
- `hs_mesh`, `hs_field`, `transition_mesh`, `transition_voxels`
- `special_points_enumerate`, `special_points_search`, `benchmark`

同步 preview kind：

- `map_image`, `raw_field`, `video_preview`, `transition_video_preview`
- `special_points_auto`, `special_points_seed`, `special_points_snap`

Orbit `formula`/`sequence` 当前可用于 2D/Julia、raw field、ln-map、HS field/mesh 和 zoom 管线。transition 是独立顶层 axis 数学，携带普通 Orbit Program 会返回 `422 UNSUPPORTED_CAPABILITY`。`weighted_schedule`、`output_blend` 和参数曲线尚未启用，以 `/capabilities` 的运行时返回为最终依据。

所有持久 kind 都通过后台 run 执行。创建响应不代表计算完成，调用方必须轮询到 `completed`、`failed` 或 `cancelled`。

## Orbit Program and safe DSL

周期 Mandelbrot/Burning Ship 示例：

```json
{
  "type": "sequence",
  "repeat": true,
  "steps": [
    {"span": 1, "program": {"type": "formula", "formula": {"type": "builtin", "id": "mandelbrot"}}},
    {"span": 1, "program": {"type": "formula", "formula": {"type": "builtin", "id": "burning_ship"}}}
  ]
}
```

DSL formula 使用 `{ "type": "dsl", "source": "z*z+c" }`。可用变量为 `z`、`c`、只读 `n` 和声明参数；常量包括 `i`、`pi`、`e`。源码、AST、嵌套、参数和单像素运算均有硬限制。编译错误包含安全错误码和字符位置。

规范化 AST 产生稳定内容 hash。外部 ln-map 复用必须与当前 Orbit hash 完全一致；不匹配的导出进入 `failed`，不会混合旧条带和新最终帧。

## Status and cancellation

状态按下列方向变化：

```text
queued -> running -> completed
                  -> failed
                  -> cancelled
```

取消是协作式的。`accepted=true` 仅表示请求已记录；worker 应继续轮询。运行状态中的 `progress.stage/current/total/percent` 用于观测，不应作为事务终态来源。

## Manifest

```json
{
  "schemaVersion": 1,
  "computeRunId": "260723-...",
  "rendererVersion": "git-or-image-version",
  "recipeHash": "sha256-of-immutable-run-snapshot",
  "status": "completed",
  "effective": {"engine": "openmp", "scalar": "fp64"},
  "hardwareExecution": {
    "requestedEngine": "auto",
    "requestedScalar": "auto",
    "actualEngine": "openmp",
    "actualScalar": "fp64",
    "hardwareClass": "cpu",
    "kernelReported": true,
    "runtimeAvailable": true,
    "engineFallback": false,
    "fallbackReason": null,
    "evidenceSource": "kernel_completion_telemetry",
    "elapsedMs": 12
  },
  "escapeAnalysis": {
    "status": "certified_finite",
    "certifiedRadius": 2.0,
    "reason": "..."
  },
  "artifacts": [
    {
      "artifactId": "run-id:result.png",
      "name": "result.png",
      "kind": "image",
      "mediaType": "image/png",
      "sizeBytes": 123,
      "sha256": "...",
      "contentPath": "/compute/v1/artifacts?artifactId=..."
    }
  ]
}
```

Platform Worker 接受完成结果前必须重新读取 manifest，要求 `status=completed`，逐个流式读取产物并验证 `sizeBytes` 与 SHA-256。artifact 路径受 run 目录约束并拒绝符号链接/目录穿越。

Benchmark 的 `hardwareExecution.mode` 为 `multi_path`，`paths[]` 分别保存每个候选的 requested/actual engine/scalar、样本和吞吐；不能把多路径测量压成一个虚构的实际 engine。

`kernelReported=true` 只在计算 kernel 完成并返回实际执行统计后出现。请求的 engine/scalar、编译能力或 CUDA 设备存在本身都不构成执行证明。回退时 `engineFallback=true` 且必须带 `fallbackReason`。

## Escape semantics

- `certified_finite`：证明器给出有限 `certifiedRadius`，允许按该半径判定数学逃逸。
- `unverified`：`certifiedRadius=null`，有限模长不能触发 escaped；只运行到迭代上限或非有限数值。
- `no_finite_bound`：已证明不存在适用的有限径向界，半径同样为 `null`。
- 溢出/NaN 只能记录为 numerical divergence/indeterminate，不能冒充 escaped。

因此无证书结果在产品界面应称为“有限迭代轨道图”。Mandelbrot/Burning Ship 输出的 50% 复数混合反例固定要求 `certifiedRadius=null`。

## Error envelope

Compute v1 错误使用结构化包络：

```json
{
  "error": {
    "code": "UNSUPPORTED_CAPABILITY",
    "message": "requested combination is not supported",
    "details": {"kind": "transition_video"}
  }
}
```

常见状态：`400` 请求/DSL/幂等键无效，`401` 服务鉴权失败，`404` run 或 artifact 不存在，`409` 资源冲突，`416` Range 不可满足，`422` 能力组合不支持，`500` 适配器或 kernel 失败。调用方只对超时和可恢复的 `5xx` 做有限重试；`4xx` 不应盲目重试。

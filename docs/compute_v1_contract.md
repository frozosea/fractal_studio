# Compute v1 私有 HTTP 合同

本文是 Platform API/Worker 与 C++ Compute v1 之间的**规范性合同**。第一次接入请先阅读 [从零调用手册](compute_v1_cookbook.md)，其中解释 Key 的生成方式、workload 选择、自定义公式、Orbit sequence、transition 和可复制的 `curl`。各 `kind` 的完整 payload、默认值、限制和产物见 [compute_v1_jobs.md](compute_v1_jobs.md)，推荐的 Worker 实现流程见 [platform_compute_integration.md](platform_compute_integration.md)。

合同版本为 `schemaVersion: 1`。除明确标为“仅供观测”的字段外，本文出现的字段均为 v1 合同的一部分。JSON 字段名区分大小写；未说明可为 `null` 的字段不得发送 `null`。

## 1. 边界与部署

- Compute v1 是私网服务，只允许 Platform Worker/受信任运维客户端访问；浏览器不得持有服务密钥。
- `GET /compute/v1/health` 是唯一无鉴权端点。
- Compute 拥有分形数学、硬件执行和本地 run 生命周期。PostgreSQL 中的用户、配方、平台任务、商业 Asset、订单和账本均由 Platform 拥有。
- Compute 的 SQLite 和 run 目录只用于节点本地运行状态，不是商业事实来源。
- URL 由 Platform 配置，例如 `http://compute:8080`。v1 没有额外的 `/api` 前缀。
- 请求和响应采用 UTF-8 JSON，二进制预览和 artifact 下载除外。

建议连接参数：连接超时 2 秒，普通 JSON 请求总超时 15 秒，artifact 流式读取超时按文件大小单独配置。运行时长不受创建请求超时控制，必须依赖轮询。

## 2. 鉴权与通用头

服务 Key 不是用户申请的 API Key，也没有申请端点。它由部署方生成，分别注入 Compute 的 `FSD_COMPUTE_SERVICE_KEY` 与 Platform API/Worker 的 `COMPUTE_SERVICE_KEY`。生成、Compose 配置和轮换方法见[调用手册第 1 节](compute_v1_cookbook.md#1-key-不是申请的)。

除 health 外必须发送：

```http
Authorization: Bearer <FSD_COMPUTE_SERVICE_KEY>
Content-Type: application/json
```

缺少、格式错误或密钥不匹配均返回：

```http
HTTP/1.1 401 Unauthorized
Content-Type: application/json

{"error":{"code":"COMPUTE_UNAUTHORIZED","message":"valid Compute service credential required","details":{}}}
```

Platform 应在自己的日志上下文中同时记录 request ID、platform job ID、compute node ID 和 `computeRunId`。Compute v1 当前不回显 Platform request ID，因此不得依赖响应头做关联。

## 3. 端点总表

| Method | Path | 成功状态 | 响应类型 | 用途 |
|---|---|---:|---|---|
| `GET` | `/compute/v1/health` | `200` | JSON | 无鉴权存活探针。 |
| `GET` | `/compute/v1/capabilities` | `200` | JSON | 能力注册表与节点硬件快照。 |
| `POST` | `/compute/v1/previews` | `200` | JSON 或 RGBA8 | 同步短预览。 |
| `POST` | `/compute/v1/runs` | `202` | JSON | 创建或复用异步持久 run。 |
| `GET` | `/compute/v1/runs/{id}` | `200` | JSON | 查询状态、进度、产物摘要和硬件证据。 |
| `POST` | `/compute/v1/runs/{id}/cancel` | `202` | JSON | 请求协作取消。请求体为 `{}`。 |
| `GET` | `/compute/v1/runs/{id}/manifest` | `200` | JSON | 读取 manifest；完成前可能为空或不完整。 |
| `GET` | `/compute/v1/artifacts?artifactId=...` | `200/206` | binary | 私有流式读取，支持单一 byte range。 |

未知路径/方法返回 `404`。调用方不能把成功创建 `202` 当成渲染完成。

## 4. Health 与能力发现

### 4.1 Health

```json
{
  "schemaVersion": 1,
  "status": "ok",
  "service": "fractal-studio-compute",
  "rendererVersion": "git-sha-or-image-version"
}
```

`status=ok` 只说明 HTTP 进程存活，不证明某个 CUDA/AVX kernel 可执行。调度必须同时读取 capabilities。

### 4.2 Capabilities

响应顶层：

| 字段 | 类型 | 含义 |
|---|---|---|
| `schemaVersion` | integer | 固定为 `1`。 |
| `rendererVersion` | string | 节点部署版本。Platform 创建任务时应保存快照。 |
| `persistentKinds` | string[] | 可用于 `/runs` 的 kind。 |
| `previewKinds` | string[] | 可用于 `/previews` 的 kind。 |
| `jobs` | object[] | 单一能力注册表，字段见下表。 |
| `orbitPrograms` | object | 当前 IR 节点开关。camelCase 键：`formula`, `sequence`, `weightedSchedule`, `outputBlend`, `axisTransition`, `axisMulti`。 |
| `orbitCompatibility` | object | Orbit Program 与产物管线的兼容快照。 |
| `customFormula` | object | `safeDsl` 与 `legacyNativeCompile` 开关。商业节点要求后者为 `false`。 |
| `escapeSemantics` | object | 当前必须为 `certifiedRadius=true`, `strictUnverified=true`。 |
| `hardware` | object | CPU/CUDA 编译与运行时快照。 |

`jobs[]`：

| 字段 | 类型 | 含义 |
|---|---|---|
| `kind` | string | 请求判别值。 |
| `persistent`, `preview` | boolean | 是否允许对应调用模式。 |
| `orbitProgram` | boolean | payload 是否允许 `orbitProgram`。 |
| `variantProfile` | string | 变体集合的稳定描述标签。 |
| `metrics`, `engines`, `scalars` | string[] | 该管线声明的候选集合；真实硬件可用性仍看 `hardware`。 |
| `outputMediaTypes` | string[] | 可能出现的响应/manifest MIME 类型。不是每次任务都必然产生全部类型。 |

硬件对象：

```json
{
  "cpu": {
    "logicalCores": 32,
    "physicalCores": 16,
    "openmp": {"compiled": true, "runtime": true},
    "avx2": {"compiled": true, "runtime": true},
    "avx512": {"compiled": false, "runtime": false}
  },
  "cuda": {
    "compiled": true,
    "runtime": true,
    "deviceCount": 1,
    "name": "...",
    "computeCapability": {"major": 8, "minor": 9},
    "totalVramBytes": 25757220864,
    "freeVramBytes": 24000000000
  }
}
```

Platform 节点登记时保存完整响应；每次调度前至少检查目标 kind、persistent/preview、Orbit 兼容性以及所需运行时。不得在 Python 中维护一份声称比节点更强的静态矩阵。

## 5. 请求包络与幂等性

预览和 run 共用包络：

```json
{
  "schemaVersion": 1,
  "kind": "map_image",
  "payload": {}
}
```

`POST /runs` 额外要求：

```json
{"idempotencyKey": "platform-job:018f..."}
```

约束：

- `schemaVersion` 必须是整数 `1`，否则 `400 UNSUPPORTED_SCHEMA_VERSION`。
- `kind` 必须是非空字符串，并属于对应的 `persistentKinds` 或 `previewKinds`。
- `payload` 必须是 JSON object。Orbit Program 只能放在 `payload.orbitProgram`，不能放在包络顶层。
- `idempotencyKey` 长度为 1–200 字符。推荐固定使用 `platform-job:<platform_job_uuid>`，包括重试和 Worker 租约重领。
- 相同 key 且 kind/payload 相同，返回第一次保存的完整创建响应和同一个 `computeRunId`。
- 相同 key 绑定不同 kind/payload 时返回 `409 IDEMPOTENCY_CONFLICT`，不会误返回另一份配方的 run。

Platform 仍必须在自己的数据库中将 key 与不可变 recipe hash/kind 绑定，以便在网络调用前发现平台一致性错误。Compute 保存规范化 payload hash，作为至少一次投递的第二道防线；升级前创建、尚无 request hash 的旧本地缓存只能继续返回原响应。

## 6. 创建、轮询与取消

### 6.1 创建响应

`POST /compute/v1/runs` 成功返回 `202`：

```json
{
  "schemaVersion": 1,
  "data": {
    "computeRunId": "260723-...",
    "kind": "map_image",
    "status": "queued",
    "legacyResult": {
      "runId": "260723-...",
      "status": "queued"
    }
  }
}
```

`data.legacyResult` 是迁移期兼容信息，Platform 不得把它映射为公共 API，也不得从其中推断终态。若底层路由同步报告 `effective`，`data.effective` 可能存在；最终实际执行信息仍以终态 status/manifest 为准。

### 6.2 状态响应

```json
{
  "schemaVersion": 1,
  "data": {
    "computeRunId": "260723-...",
    "status": "running",
    "module": "video-export",
    "startedAt": 1784772000123,
    "finishedAt": 0,
    "cancelRequested": false,
    "progress": {
      "taskType": "video_export",
      "stage": "frames",
      "current": 12,
      "total": 120,
      "percent": 10.0,
      "elapsedMs": 934.5,
      "cancelable": true,
      "kernelReported": false,
      "resourceLocks": ["video_export", "cpu_heavy"],
      "details": {}
    },
    "hardwareExecution": {},
    "artifacts": [
      {"artifactId": "run-id:start_frame.png", "name": "start-frame", "kind": "image"}
    ]
  }
}
```

- `status` 枚举：`queued`, `running`, `completed`, `failed`, `cancelled`。
- 合法方向是 `queued -> running -> terminal`；极快任务也可能直接观察到 terminal。
- `startedAt`/`finishedAt` 是 Unix epoch 毫秒；未完成时 `finishedAt=0`。
- `progress` 是按 job 扩展的观测对象，字段会随 kind/stage 改变。`stage` 通常存在；`current/total/percent/estimatedRemainingMs/cancelable/kernelReported` 均应按 optional 建模，special-points 等阶段可能没有通用 percent。Platform 应保存原对象，并按 [调用手册的进度规则](compute_v1_cookbook.md#34-进度条怎样做)生成 UI 进度，不能要求每个 kind 都有相同字段。
- `artifacts` 是当前已登记的摘要，任务运行中可以增长。只有 terminal manifest 才可作为接收清单。
- HTTP `200` 不表示任务成功；必须检查 `data.status`。

### 6.3 取消

请求体固定为 `{}`。返回 `202`：

```json
{
  "schemaVersion": 1,
  "data": {
    "computeRunId": "260723-...",
    "status": "running",
    "accepted": true,
    "cancelRequested": true
  }
}
```

`accepted=true` 只表示取消标记已记录。Worker 必须继续轮询，直到 `cancelled`、`completed` 或 `failed`。完成与取消竞争时 `completed` 是合法结果；Platform 应把用户取消意图和 Compute 实际终态分别保存。

### 6.4 重启恢复

节点重启会把遗留的 `queued/running` 本地 run 协调到 terminal 状态。Platform 租约重领后应先用已保存的 `computeRunId` 查询，只有明确 `404` 且创建结果没有持久化时才用原幂等键重新提交。

## 7. Manifest 与产物接收

Manifest 在任意已存在 run 上可读取；**只有 `status=completed` 时可接收商业产物**：

```json
{
  "schemaVersion": 1,
  "computeRunId": "260723-...",
  "rendererVersion": "git-or-image-version",
  "recipeHash": "64-lowercase-hex",
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
    "elapsedMs": 12.4
  },
  "escapeAnalysis": {
    "status": "certified_finite",
    "certifiedRadius": 2.0,
    "reason": "..."
  },
  "artifacts": [
    {
      "artifactId": "run-id:map.png",
      "name": "map",
      "kind": "image",
      "mediaType": "image/png",
      "sizeBytes": 123456,
      "sha256": "64-lowercase-hex",
      "contentPath": "/compute/v1/artifacts?artifactId=run-id:map.png"
    }
  ]
}
```

`recipeHash` 是 Compute 保存的原始 payload JSON 文本的 SHA-256，用于审计，不等于 Platform 的规范化 recipe hash；Platform 应同时保存两者。`contentPath` 是同节点相对路径，必须与产生 manifest 的 `compute_node_id` 绑定，不能拼到任意节点。

Worker 接收算法：

1. 只在 status 为 `completed` 后读取 manifest。
2. 校验 schema、run ID、renderer version、非空 artifacts，以及该 kind 的必需产物集合。
3. 校验硬件执行证据（第 8 节）。
4. 对每个 artifact 使用服务鉴权流式 GET；不得信任 `name` 作为本地路径。
5. 边下载边计算 SHA-256 并累计字节数，与 manifest 严格比较。
6. 上传对象存储后再提交 Platform DB 终态；任一校验失败都不得创建商业 Asset。
7. 至少一次消费下，已接收的 `(compute_node_id, computeRunId, artifactId, sha256)` 必须幂等。

### Artifact HTTP

- 完整读取返回 `200`, `Content-Type`, `Content-Length`, `Accept-Ranges: bytes`。
- `Range: bytes=START-END`、`bytes=START-` 或后缀范围返回 `206` 和 `Content-Range`；只支持单一范围。
- 无效/越界/多范围返回 `416`，并带 `Content-Range: bytes */<size>`。
- 当前没有 ETag/Last-Modified。发送 `If-Range` 时服务端安全地返回完整 `200`，调用方应丢弃旧分片并重算 hash。
- artifact ID 不存在返回 `404`。路径穿越、目录和符号链接不会被读取。

## 8. 硬件执行证据

结果准确之外，Platform 必须验证请求的计算硬件确实执行了 kernel。

普通任务的 `hardwareExecution` 字段如 manifest 示例。验收规则：

- 必须 `kernelReported=true` 且 `evidenceSource=kernel_completion_telemetry`；仅 `capabilities.hardware.cuda.runtime=true` 不构成执行证明。
- `actualEngine`/`actualScalar` 必须非空，并保存到平台任务和配方快照。
- `hardwareClass` 为 `cpu`, `gpu`, `hybrid`, `unknown`。商业任务不得在未明确产品策略时接受 `unknown`。
- `runtimeAvailable` 必须为 true。
- 若产品请求特定 engine，`engineFallback=true` 应按产品策略拒绝或标为降级；`auto` 允许选择任一语义等价路径。
- scalar/engine 回退不得改变数学语义。不支持的数学组合必须在创建时失败，不能静默替换为 Mandelbrot。

`benchmark` 使用多路径结构：

```json
{
  "mode": "multi_path",
  "kernelReported": true,
  "evidenceSource": "benchmark_candidate_telemetry",
  "paths": [
    {
      "requestedEngine": "cuda",
      "requestedScalar": "fp64",
      "actualEngine": "cuda",
      "actualScalar": "fp64",
      "available": true,
      "sampleElapsedMs": [1.2, 1.1, 1.3]
    }
  ],
  "elapsedMs": 42.0
}
```

不得把 `paths[]` 压成一个虚构的 actual engine。每个候选只有 requested 与 actual 完全匹配并收齐样本时才是 `available=true`。

## 9. 严格逃逸语义

| `escapeAnalysis.status` | `certifiedRadius` | 语义 |
|---|---:|---|
| `certified_finite` | finite number | 证明器给出有限半径，可据此判定数学逃逸。 |
| `unverified` | `null` | 未证明。任何有限模长都不能仅凭阈值标为 escaped。 |
| `no_finite_bound` | `null` | 已知没有适用的有限径向界。 |

无证书轨道只运行到 `maxIterations` 或遇到非有限数值；溢出/NaN 是 `numerically_diverged/indeterminate`，不是数学逃逸。任意 DSL、transcendental、未来 output blend 默认无证书。Mandelbrot/Burning Ship 的离散周期 sequence 可组合共同证书；两者复数输出的 50% blend 反例必须保持 `certifiedRadius=null`。

Platform 保存完整证明对象。面向用户时，无证书结果应称为“有限迭代轨道图”。

## 10. 错误、重试与失败归属

Compute v1 自身错误采用：

```json
{
  "error": {
    "code": "UNSUPPORTED_CAPABILITY",
    "message": "requested combination is not supported",
    "details": {"kind": "transition_video"}
  }
}
```

稳定错误码：

| HTTP | code | 含义/处理 |
|---:|---|---|
| `400` | `UNSUPPORTED_SCHEMA_VERSION` | 客户端合同版本错误，不重试。 |
| `400` | `INVALID_REQUEST` | 缺 kind/payload 或字段验证失败，不重试。 |
| `400` | `INVALID_IDEMPOTENCY_KEY` | key 缺失或超过 200 字符，不重试。 |
| `400` | `INVALID_ORBIT_PROGRAM` 或 DSL 编译码 | IR/DSL 无效；`details.position` 是从 0 开始的源码字符偏移。 |
| `401` | `COMPUTE_UNAUTHORIZED` | 节点凭据配置错误，告警且不盲重试。 |
| `404` | 资源不存在 | run/artifact/node 路由不存在。 |
| `409` | 资源锁冲突 | 节点重任务已占用；保留同一平台任务并退避重试调度。 |
| `409` | `IDEMPOTENCY_CONFLICT` | 同一 key 对应不同 kind/payload；平台一致性故障，不重试。 |
| `416` | 无 JSON body | Range 不可满足；重新完整下载。 |
| `422` | `UNSUPPORTED_CAPABILITY` | kind、Orbit、metric 或数学/输出组合不支持，不重试。 |
| `500` | `COMPUTE_ADAPTER_ERROR` | 适配层没有产生合法响应；节点故障。 |

迁移期底层计算路由的部分 `400/409/500` body 仍可能是 `{ "error": "text", ... }`。ComputeClient 应先解析规范 envelope，失败时保留 HTTP status、原始 body 和节点 ID为内部错误；公共 API 不得原样泄漏内部路径或命令输出。

创建请求只对连接失败、超时和可恢复 `5xx` 做有限重试，并始终复用同一幂等键。建议指数退避加抖动：1、2、4 秒，最多 3 次。轮询的 `404`、业务 `4xx` 和 terminal `failed` 不自动创建新 run。

## 11. Orbit Program 与安全 DSL

单公式：

```json
{"type":"formula","formula":{"type":"builtin","id":"mandelbrot"}}
```

周期序列：

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

`span` 是正整数连续执行次数；序列按每像素相同的确定性日程循环到 `iterations`。当前启用 `formula` 和 `sequence`；`weighted_schedule`、`output_blend` 和参数曲线尚未启用。transition 使用独立 axis 数学，不能附带普通 Orbit Program。

安全 DSL：

```json
{
  "type": "formula",
  "formula": {
    "type": "dsl",
    "source": "z*z+c",
    "parameters": {}
  }
}
```

可用变量为 `z`, `c`, 只读 `n` 和声明参数；常量为 `i`, `pi`, `e`。DSL 禁止赋值、循环、递归、文件、网络、动态符号和任意原生调用。限制为源码 4 KiB、AST 256 节点、嵌套 32、参数最多 16 个，并有单像素运算预算。规范化 AST 产生稳定 hash；复用 ln-map 时 Orbit hash 必须完全一致。

## 12. Platform 实现验收清单

- 启动时 health + capabilities 成功，生产节点 `legacyNativeCompile=false`。
- ComputeClient 对 JSON、RGBA8 和流式 artifact 使用三种明确响应类型。
- 创建和 Outbox 重投始终复用稳定幂等键。
- 已保存 `computeRunId` 的任务先恢复轮询，不重复创建。
- 取消后继续轮询，正确处理 cancel/completion race。
- 只接受 completed manifest，逐项校验 size/SHA-256/必需产物。
- 保存 rendererVersion、capability snapshot、effective engine/scalar、hardwareExecution、escapeAnalysis。
- 对特定硬件产品校验 kernel 执行证据，而非只看设备存在。
- `422` 不降级数学；`4xx` 不盲重试；内部错误不泄漏给浏览器。
- 18 个 kind 的 payload 与产物均按 [任务参考](compute_v1_jobs.md) 建模或安全透传。

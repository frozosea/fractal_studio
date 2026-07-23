# Platform Backend 对接 Compute v1 指南

本文给 FastAPI 服务后端开发者一条可直接实现的最小闭环。公共商业 API 和领域字段以 `platform-backend-spec.zh.pdf` 为准；本文只规定 Platform 内部如何调度 C++ Compute。第一次手动调用、Key 配置和公式/transition 示例见[从零调用手册](compute_v1_cookbook.md)，HTTP 合同见 [compute_v1_contract.md](compute_v1_contract.md)，任务参数见 [compute_v1_jobs.md](compute_v1_jobs.md)。

## 1. Platform 内部组件

```text
Browser
  -> FastAPI route/service
     -> PostgreSQL transaction:
          render_jobs + quota_reservations + outbox_events

Outbox Worker
  -> claim event (FOR UPDATE SKIP LOCKED)
  -> select Compute node from capability snapshot
  -> POST /compute/v1/runs
  -> persist node_id + compute_run_id
  -> poll /runs/{id}
  -> GET manifest
  -> verify hardware + artifacts
  -> stream artifact -> object storage
  -> PostgreSQL terminal transaction
```

Redis 只用于限流和配额计数，不作为渲染队列。Outbox 是唯一后台任务来源。

## 2. 建议的强类型 DTO

服务端应把运输合同与 18 类 recipe payload 分开。payload 可用 Pydantic discriminated union 建模；第一阶段也可以在完成 kind allowlist 和大小限制后保存为 `dict[str, Any]`，但发送时必须来自不可变 recipe snapshot，不能接收浏览器任意 JSON 直通私网。

```python
from typing import Any, Literal
from pydantic import BaseModel, ConfigDict, Field


class StrictDto(BaseModel):
    model_config = ConfigDict(extra="forbid")


class ComputeRunRequest(StrictDto):
    schemaVersion: Literal[1] = 1
    kind: str
    idempotencyKey: str = Field(min_length=1, max_length=200)
    payload: dict[str, Any]


class ComputeArtifact(StrictDto):
    artifactId: str
    name: str
    kind: str
    mediaType: str
    sizeBytes: int = Field(ge=0)
    sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    contentPath: str


class ComputeErrorBody(BaseModel):
    # 迁移期底层错误尚不全是统一 envelope，HTTP client 需保留 raw body。
    error: dict[str, Any] | str
```

响应模型建议 `extra="allow"`：Compute 的 `progress.details` 和某些 preview 数据会向后兼容地增加观测字段，Transport 客户端不应因此拒绝整个响应。请求模型保持 `extra="forbid"`，防止把 Platform 内部字段误发给 Compute。

`map_image` preview 必须使用独立方法返回 `bytes + headers`；不要让通用 JSON 方法尝试解析它。artifact 使用 `httpx.AsyncClient.stream()`，不能一次性读入内存。

## 3. ComputeClient 接口

建议只暴露以下内部方法：

```python
class ComputeClient:
    async def health(self) -> Health: ...
    async def capabilities(self) -> Capabilities: ...
    async def preview_json(self, kind: str, payload: dict) -> dict: ...
    async def preview_rgba(self, payload: dict) -> RgbaFrame: ...
    async def create_run(self, request: ComputeRunRequest) -> CreatedRun: ...
    async def get_run(self, run_id: str) -> ComputeRun: ...
    async def cancel_run(self, run_id: str) -> CancelResult: ...
    async def get_manifest(self, run_id: str) -> ComputeManifest: ...
    async def stream_artifact(self, artifact_id: str, range_: str | None = None): ...
```

所有方法自动添加 bearer 密钥；调用者不能自行提供 Authorization。base URL 和密钥按 `compute_node_id` 从受保护配置读取，密钥不得写入数据库事件、日志或异常字符串。

有限重试只能包围单次 HTTP 操作：连接失败/超时/502/503/504 可按 1、2、4 秒加抖动重试，`POST /runs` 始终复用原 idempotency key。不要在 client 内部等待任务完成；轮询属于 Worker 状态机。

## 4. PostgreSQL 最小持久状态

`render_jobs` 至少保存：

| 字段 | 用途 |
|---|---|
| `id`, `user_id`, `recipe_snapshot_id`, `kind` | 平台身份与不可变输入 |
| `status`, `cancel_requested_at` | 平台状态和用户意图 |
| `idempotency_key` | 固定 `platform-job:<uuid>`，unique |
| `request_payload`, `platform_recipe_hash` | 实际发送快照和 Platform 规范化 hash |
| `compute_node_id`, `compute_run_id` | 必须成对持久化；run ID 在该节点命名空间内 |
| `compute_renderer_version`, `capability_snapshot` | 可复现性与节点选择证据 |
| `compute_status`, `compute_progress` | 最近一次轮询原文 |
| `compute_recipe_hash`, `effective_engine`, `effective_scalar` | terminal manifest 数据 |
| `hardware_execution`, `escape_analysis`, `compute_manifest` | 完整审计对象 |
| `error_code`, `error_message_internal` | 内部失败；公共错误另做安全映射 |
| `lease_owner`, `lease_expires_at`, `attempt_count`, `next_attempt_at` | Outbox/Worker 恢复 |
| `created_at`, `started_at`, `finished_at` | Platform UTC 时间；Compute epoch ms 可另存 |

不要把 `legacyResult`、legacy artifact URL 或 Compute 本地绝对路径当业务字段。

## 5. 创建事务

API 收到合法公共请求后在**一个 PostgreSQL 事务**中：

1. 锁定/检查额度并写 `quota_reservations`。
2. 固化 recipe snapshot，计算 Platform recipe hash。
3. 创建 `render_jobs(status='queued')` 和稳定 idempotency key。
4. 创建唯一 outbox event，例如 `(aggregate_id, event_type)` unique。
5. 提交事务后立即向浏览器返回 platform job ID；API 进程不直接调用 Compute。

重复公共请求按 PDF 的公共幂等规则返回同一 platform job。Outbox 重复投递也必须落到同一 Compute idempotency key。

## 6. Worker 状态机

### 6.1 Claim

在短事务中使用 `FOR UPDATE SKIP LOCKED` 领取到期 event，写 `lease_owner/lease_expires_at` 后立即提交。计算和网络请求不得持有数据库行锁。租约必须可过期重领。

### 6.2 Submit 或 resume

```text
if compute_node_id and compute_run_id exist:
    resume polling that exact node/run
else:
    choose node whose snapshot supports kind/mode/orbit/hardware policy
    POST /runs with stable key and immutable payload
    transactionally save node_id + run_id + renderer/capability snapshot
```

保存 run ID 失败后 Worker 可能重复 `POST /runs`；Compute 幂等键会返回同一 run。若相同平台 key 对应的 kind/hash 不同，应在 Platform 本地标为一致性故障，不发送请求。

### 6.3 Poll

建议初始 250–500 ms，逐步上升到 2–5 秒并加抖动。每次轮询：

- 保存 `compute_status` 和完整 progress；
- 第一次观察到 running 时设置 Platform `started_at`；
- 若平台已收到取消意图且尚未转发，调用 cancel 一次并记录结果；
- `queued/running`：续租或安排下一次短轮询；
- `completed`：进入 manifest 接收；
- `failed/cancelled`：进入 terminal transaction。

不要用 `progress.percent==100`、artifact 出现或 `kernelReported` 单独判断完成。

### 6.4 Cancel race

用户取消只更新 Platform job + outbox，不直接从 API route 阻塞等待 Compute。Worker 转发取消后继续轮询：

| Compute terminal | Platform 处理 |
|---|---|
| `cancelled` | 释放额度/按策略结算，终态 cancelled。 |
| `completed` | 记录“取消晚于完成”；可接收或丢弃产物由产品策略决定，但事实终态不可伪造成 Compute cancelled。 |
| `failed` | 保存真实失败；同时保留取消意图。 |

### 6.5 Failed run

Compute terminal `failed` 通常通过 `GET /runs/{id}` 返回 HTTP 200。失败详情优先从 `progress.errorMessage`、`progress.details.error`、`progress.failedStage` 读取并原样保存到内部字段。给浏览器使用稳定的 Platform 错误码，不泄漏路径、FFmpeg stderr 或节点配置。

## 7. Manifest 和 artifact 接收

建议单独实现 `verify_manifest(kind, expected, manifest)`：

```text
schemaVersion == 1
computeRunId == saved compute_run_id
status == completed
rendererVersion non-empty
recipeHash matches 64 lowercase hex
required artifact set for kind is present
every artifactId is non-empty and unique
every sizeBytes >= 0 and sha256 is 64 lowercase hex
hardware policy passes
escapeAnalysis shape is valid
```

各 kind 必需文件见任务参考。对于文件名可变的视频，使用 `mediaType=video/mp4` + kind 验证；固定 sidecar/关键帧仍验证文件名。

流式搬运伪代码：

```python
hasher = hashlib.sha256()
size = 0
async with client.stream_artifact(item.artifactId) as response:
    response.raise_for_status()
    async for chunk in response.aiter_bytes():
        size += len(chunk)
        hasher.update(chunk)
        await object_writer.write(chunk)

if size != item.sizeBytes or hasher.hexdigest() != item.sha256:
    await object_writer.abort()
    raise ArtifactIntegrityError(...)
await object_writer.commit()
```

对象 key 必须由 Platform 自己生成，例如 `jobs/<platform_job_id>/<artifact_uuid>`；不要直接使用 Compute `name`。上传成功后在一个事务中写接收记录、manifest、终态和 Outbox 后续事件。失败重试时按 sha256 幂等复用或清理未提交 multipart upload。

底座阶段只保存 manifest，不创建商业 Asset；M3 的资产摄取在校验闭环之后执行。

## 8. 硬件策略实现

节点选择前验证 capability snapshot；任务完成后再次验证 `hardwareExecution`：

```python
def verify_hardware(evidence, required_class=None, required_engine=None):
    if evidence.kernelReported is not True:
        raise HardwareEvidenceError("kernel did not report completion")
    if evidence.runtimeAvailable is not True:
        raise HardwareEvidenceError("reported runtime unavailable")
    if not evidence.actualEngine or not evidence.actualScalar:
        raise HardwareEvidenceError("missing actual path")
    if required_class and evidence.hardwareClass != required_class:
        raise HardwareEvidenceError("wrong hardware class")
    if required_engine and evidence.actualEngine != required_engine:
        raise HardwareEvidenceError("requested engine was not used")
```

`engine=auto` 的普通渲染可接受语义等价 actual path，但必须记录。付费 GPU SKU 则应要求 `hardwareClass in {'gpu','hybrid'}`，并根据产品定义决定 hybrid 是否合格。`cudaWarp=true`、`videoEncoder=h264_nvenc` 或节点存在 CUDA 都不等价于 fractal kernel 使用 CUDA。

Benchmark 逐项验证 `paths[]`；只有 requested/actual 完全匹配、`available=true`、样本数符合请求的路径能更新节点性能评分。

## 9. Preview 代理

预览仍应通过 FastAPI：

- 鉴权/配额/限流在 Platform 完成；浏览器只持有 Cookie session。
- Platform 从 immutable draft 构造 allowlisted Compute payload，设置较小尺寸/迭代/超时。
- JSON preview 去掉 legacy `/api/*` URL；如需图片，Platform 用 artifact ID 拉取后代理或生成受控临时 URL。
- RGBA preview 校验响应头、尺寸和 body 长度，再返回浏览器。
- 同步 preview 超时不转成持久 run；浏览器可重新请求，Platform 按预览策略限流。

固定测试主体只允许 development/test 开关。生产未完成 M1 时，Studio 路由保持关闭。

## 10. HTTP 到 Platform 错误映射

| Compute 结果 | Platform 内部分类 | 是否自动重试 |
|---|---|:---:|
| connect/timeout, 502/503/504 | `compute_temporarily_unavailable` | 有限重试 |
| 400 validation/DSL | `invalid_compute_recipe` | no |
| 401 | `compute_node_misconfigured` + alert | no |
| 404 before run ID saved | `compute_submission_unknown` | 原 key 重提一次 |
| 404 after run ID saved | `compute_run_lost` + alert | no new run automatically |
| 409 resource lock | `compute_node_busy` | yes, reschedule |
| 422 | `unsupported_compute_capability` | no |
| terminal failed | `compute_execution_failed` | 产品策略决定，不能换数学静默重跑 |
| artifact size/hash mismatch | `compute_artifact_integrity_failed` + alert | 可重下载；不可发布 |
| hardware evidence mismatch | `compute_hardware_policy_failed` + alert | 不可发布 |

公共 API 的状态码和响应包络仍按商业 PDF，不要把 Compute 的内部 HTTP 状态一比一暴露。

## 11. 最小集成验收

服务后端完成 Compute 对接至少要有真实 HTTP 集成测试：

1. 无密钥 capabilities 为 401，health 无密钥为 200。
2. FastAPI 创建 preview 并收到合法 RGBA/JSON。
3. 一个平台 job 经 PostgreSQL Outbox 提交真实 `map_image` run，重投仍是同一 Compute run。
4. Worker 跨进程/租约重领后使用已保存 run ID 恢复轮询。
5. completed manifest 的全部 artifact 经真实 HTTP 下载并校验 size/SHA-256。
6. 制造长任务、转发取消并观察 terminal；另测取消/完成竞争。
7. 指定 CPU/GPU 策略，既验证结果又断言 `hardwareExecution` 的 actual path。
8. 对 18 个 kind 至少做合同级 payload/能力测试；当前持久 kind 全部跑一次真实 run。
9. 422 组合不会回退成 Mandelbrot，失败 progress 会进入 Platform 内部错误。
10. 浏览器请求链路中不存在 C++ base URL 或服务密钥。

仓库已有 Compute 自身的真实 HTTP pytest 套件；Platform 测试应把 API、Worker、PostgreSQL 和 Compute 一起放入 Compose，而不是只 mock `ComputeClient`。

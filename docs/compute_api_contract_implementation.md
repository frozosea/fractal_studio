# C++ Compute `/api` 生产合同实施记录

本文记录 `compute_api_contract` 分支对 Platform Backend 私有计算合同的实施状态。权威调用方是 `platform_backend@81bc3fc` 中的：

- `platform-backend/docs/compute-openapi.yaml`
- `platform-backend/app/infrastructure/compute/compute_client.py`
- `platform-backend/app/studio/compute_request_mapper.py`
- `platform-backend/app/studio/render_worker.py`

本分支不修改 Platform 领域、路由或 Worker。所有实现变更限定在 C++ `backend/`、计算合同文档和跨端测试。

最后更新：2026-07-24

## 1. 已确认的路径归属

```text
Browser
  -> Platform Backend public /v1/*
     -> PostgreSQL + Outbox Worker
        -> C++ Compute private /api/*
```

Platform 当前实际调用以下 7 条 Compute 路径：

| Method | Path | Platform 调用点 |
|---|---|---|
| POST | `/api/map/render-inline` | bounded preview |
| POST | `/api/map/render` | durable PNG |
| POST | `/api/video/export` | durable MP4 |
| POST | `/api/hs/mesh` | durable GLB/STL |
| POST | `/api/transition/mesh` | durable transition GLB/STL |
| GET | `/api/runs/status?runId=...` | Worker poll |
| POST | `/api/runs/cancel` | Worker cancellation |

现有 `/compute/v1/*` 是已经实现的另一套 C++ 私有适配器。迁移期间保留它以避免破坏现有测试和工具，但 Platform 不需要为它改写。等 `/api/*` 生产合同完成真实集成验收后，再单独决定是否删除。

## 2. 双轨规则

上述 7 条路径与本地旧前端使用的 C++ legacy `/api/*` 同名。服务端按以下规则分流：

1. 商业/托管配置关闭 `FSD_ENABLE_LEGACY_API`；7 条路径只接受有效 Compute service Bearer key，返回生产合同。
2. 本地迁移配置允许 legacy 时：没有 `Authorization` 的请求继续旧语义；携带 `Authorization` 的请求必须通过认证并走生产合同，错误 key 返回 401，不能降级到 legacy。
3. 其他 legacy `/api/*` 路径不属于生产合同，继续受 legacy 开关控制。
4. `/compute/v1/*` 继续使用同一 service key，但不作为 Platform Backend 的验收目标。

## 3. 合同适配设计

生产 adapter 复用现有 Compute v1 的安全能力、异步 run、幂等缓存、manifest 和硬件执行证据，不复制 kernel：

```text
Platform /api request
  -> strict production validator
  -> field/enum translation
  -> existing Compute run/preview implementation
  -> flat RunAccepted / RunStatus / Problem response
```

关键转换：

- `clientJobId` 作为 Compute 幂等键；相同 ID/相同规范请求复用 run，不同请求返回 409。
- Platform `engine=cpu` 转为 `openmp`；`cuda` 保持；`auto` 保持。
- Platform `scalarType=float/double/long_double` 转为 `fp32/fp64/fp80`。
- `durationSeconds` 转为现有视频 payload 的 `durationSec`。
- durable map 强制 `stillExport=true`、`background=true`；调用方不能改成同步任务。
- flat response 使用 `runId/clientJobId/status/progressPercent/artifacts/error`，不暴露 `legacyResult`、本地路径或 Compute v1 包络。
- artifact 只返回 OpenAPI 允许的 PNG、MP4、GLB、STL；内部 JSON、progress 和临时文件不进入 Platform RunStatus。

## 4. 错误与安全

所有生产 `/api` 错误使用 OpenAPI 的 flat Problem：

```json
{
  "code": "invalid_request",
  "message": "width must be in 1..4096",
  "requestId": "00000000-0000-4000-8000-000000000000"
}
```

- 400：JSON、必填字段、类型、enum 或取值错误。
- 401：缺少或错误 service key。
- 403：预留给未来 key scope；当前单 key 模型不会伪造 scope。
- 404：未知 run。
- 409：幂等冲突、非法 run 状态或资源冲突。
- 413：请求尺寸/迭代/产物上限超过合同。
- 429：预留给 Compute 速率限制；未实现 limiter 前不得错误宣称已限流。
- 500：内部失败；响应不得泄漏路径、SQLite、FFmpeg stderr 或异常栈。

## 5. 实施任务与进度

| Scope | Status | Acceptance |
|---|---|---|
| 分支与合同基线 | completed | 基于 `platform_backend@81bc3fc` 创建；确认 Platform 实际使用 7 条 `/api/*`。 |
| 实施文档 | completed | 路径归属、双轨、字段转换、错误和测试策略已记录。 |
| 认证与 dispatch | pending | hosted `/api` 强制 Bearer；本地无 header legacy 双轨；错误 key 不降级。 |
| strict DTO/Problem | pending | OpenAPI required/limit/enum 和 flat Problem 全覆盖。 |
| preview adapter | pending | RGBA 长度/headers/1–1024 受限预览符合合同。 |
| durable adapters | pending | image/video/HS/transition 均异步、幂等并返回 flat RunAccepted。 |
| status/cancel | pending | flat RunStatus、artifact 过滤、404 和 cancel poll 语义符合合同。 |
| Platform HTTP integration | pending | 不 mock ComputeClient，真实 Platform DTO 请求 C++ 并完成提交/轮询/取消。 |
| 回归与文档 | pending | Compute pytest、CTest、Platform tests 和合同检查全部通过。 |

## 6. 测试策略

测试继续采用 fixture 启动真实 C++ HTTP 进程，每个测试独立：

- 7 条 method/path 与 OpenAPI 一致。
- 无 key、错误 key、有效 key，以及 legacy 开关四种组合。
- 每类 durable payload 来自 Platform `compute_request_mapper.py` 的真实字段。
- 同一 `clientJobId` 重投返回同一 run；改变 payload 返回 409。
- status 从 queued/running 到 terminal，字段可被 Platform `ComputeClient._run_status` 解析。
- completed artifact 类型、大小、ID 符合 allowlist；不出现本地路径。
- 取消后继续轮询到 terminal；不存在 run 返回 404 Problem。
- malformed JSON、缺字段、错误 enum、边界值和超限分别返回 400/413。
- 旧无认证本地路由在 legacy 开关打开时保持回归；商业关闭时不可访问。

最终增加至少一条跨端测试：使用 `platform-backend/app/infrastructure/compute/compute_client.py` 对真实 C++ 进程执行 preview 和一个 durable run，证明双方不是各自只测 mock。

## 7. 明确不做

- 不修改 Platform 公共 `/v1/*`。
- 不在 FastAPI 中新增这些 `/api/*`。
- 不改 Platform Recipe、Outbox、Worker 或领域数据库结构。
- 不在本批次删除 `/compute/v1/*`。
- 不把 C++ legacy 响应、绝对路径或 service key暴露给浏览器。

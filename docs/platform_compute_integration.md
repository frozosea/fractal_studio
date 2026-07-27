# Platform Backend 对接 C++ Compute 指南

本文只描述当前 `platform_backend@81bc3fc` 已实现、C++ 已验证的生产调用链。不要把
`/compute/v1/*` 的通用 Compute v1 工具合同误当成当前 Platform Worker 合同。

## 1. 权威来源与边界

发生冲突时按以下顺序判断：

1. [`platform-backend/docs/compute-openapi.yaml`](../platform-backend/docs/compute-openapi.yaml)：生产 HTTP 合同。
2. [`ComputeClient`](../platform-backend/app/infrastructure/compute/compute_client.py)：当前真实调用与错误映射。
3. [`compute_request_mapper.py`](../platform-backend/app/studio/compute_request_mapper.py)：Recipe/Output 到 Compute body 的字段映射。
4. [`render_worker.py`](../platform-backend/app/studio/render_worker.py)：提交、轮询与取消状态机。
5. C++ adapter：[`routes_platform_compute.cpp`](../backend/src/api/routes_platform_compute.cpp)。

浏览器公共接口属于 FastAPI `/v1/*`；C++ `/api/*` 是私网服务间接口：

```text
Browser -> FastAPI /v1/* -> PostgreSQL Outbox -> Worker
       Worker -> C++ Compute private /api/*
```

生产浏览器不能获得 Compute base URL 或 service key。旧 Vue 直连 C++ 只属于本地迁移模式。

## 2. Key 从哪里来

Compute key 不是用户申请的 API key，也没有申请接口。部署人员生成一个随机服务密钥，并把
同一个值分别注入：

```bash
export COMPUTE_SERVICE_KEY="$(openssl rand -hex 32)"        # Platform API/Worker
export FSD_COMPUTE_SERVICE_KEY="$COMPUTE_SERVICE_KEY"       # C++ Compute
```

Platform 配置还需要 `COMPUTE_BASE_URL=http://compute.internal:18080`。所有生产路由请求都带：

```http
Authorization: Bearer <service-key>
```

错误/缺失 key 返回 `401` flat Problem。密钥不得进入数据库、Outbox payload、日志或浏览器构建。
本地旧前端直接运行根目录 `./dev.sh`；脚本会生成一次性开发值并同时注入 Vite/C++，不落盘、不打印。

## 3. 当前 7 条生产路由

| Method | Path | 用途 | 成功响应 |
|---|---|---|---|
| POST | `/api/map/render-inline` | 有界 RGBA8 预览 | 200 binary / 204 |
| POST | `/api/map/render` | 异步 PNG | flat `RunAccepted` |
| POST | `/api/video/export` | 异步 MP4 | flat `RunAccepted` |
| POST | `/api/hs/mesh` | 异步 HS GLB/STL | flat `RunAccepted` |
| POST | `/api/transition/mesh` | 异步 transition GLB/STL | flat `RunAccepted` |
| GET | `/api/runs/status?runId=...` | Worker 轮询 | flat `RunStatus` |
| POST | `/api/runs/cancel` | 转发取消意图 | `cancel_requested` |

生产响应没有 `schemaVersion/data` 包络，也没有 `computeRunId` 字段；实际字段名是 `runId`。

### 3.1 Preview

Platform 必须用 `map_preview_v1()` 生成 body。`requestId` 为 UUID；`engine` 取
`auto/cpu/cuda`，`scalarType` 取 `auto/float/double/long_double`。C++ 会映射到实际
`openmp/cuda` 与 `fp32/fp64/fp80`。

当前 mapper 只转发内置 `colorMap`。C++ 已支持二维 `colorProgram` v1，但在协作者扩展 Recipe schema、
生产 OpenAPI 和 mapper 前，Platform 公共 API 不得声称已经支持自定义染色；任务与 schema 见
[染色合同](coloring_contract.md)。

成功时必须同时校验：

- `Content-Type: application/octet-stream`
- `X-FSD-Width`、`X-FSD-Height`
- `X-FSD-Pixel-Format: rgba8`
- body 长度严格等于 `width * height * 4`

预览最大宽高均为 1024。204 表示被抢占/取消，不得当作空白成功图。

### 3.2 Durable request

所有 durable body 都必须包含 UUID `clientJobId`，它就是 Platform `render_jobs.id`，也是
Compute 幂等键。相同 ID + 相同规范 body 返回同一 `runId`；相同 ID + 不同 body 返回 409。

请求必须由 `map_durable_v1()` 从不可变 recipe snapshot 和 output spec 生成，禁止浏览器 JSON
直接透传。四类映射：

- image：map 字段 + `clientJobId` + `stillExport=true` + `background=true`。
- video：map 字段 + `clientJobId/fps/durationSeconds`；C++ 内部转成 `durationSec`。
- HS：`centerRe/centerIm/scale/resolution/iterations/variant/bailout` 加可选高度参数。
- transition：`centerX/Y/Z/extent/resolution/iterations/transitionFrom/To` 加可选 mesh 参数。

注意：当前 FFmpeg 链要求视频宽高至少 128；Platform Pydantic/OpenAPI 暂允许 1..127，这是已知
跨端合同差异。C++ 会明确返回 400，不会静默放大。Platform 后续应把视频下界收紧到 128。

## 4. 响应 DTO

创建成功：

```json
{
  "runId": "...",
  "clientJobId": "00000000-0000-4000-8000-000000000000",
  "status": "queued"
}
```

轮询成功：

```json
{
  "runId": "...",
  "clientJobId": "00000000-0000-4000-8000-000000000000",
  "status": "completed",
  "progressPercent": 100,
  "artifacts": [
    {
      "artifactId": "run-id:map.png",
      "purpose": "master",
      "mediaType": "image/png",
      "sizeBytes": 1234
    }
  ]
}
```

允许的 artifact media type 只有 `image/png`、`video/mp4`、`model/gltf-binary`、`model/stl`；
本地路径、progress JSON、FFmpeg stderr 和 Compute v1 manifest 不会进入该 DTO。

错误统一为：

```json
{
  "code": "invalid_request",
  "message": "safe message",
  "requestId": "00000000-0000-4000-8000-000000000000"
}
```

Platform 当前 client 将 401/403 映射为 `compute_auth_failed`，404 为 `compute_run_not_found`，
409 为 `compute_conflict`，5xx/连接失败为 `compute_unavailable`，其他 4xx 为 `compute_rejected`。

## 5. Worker 状态机

Outbox 仍是唯一后台执行器，Redis 不是渲染队列。领取事件使用
`FOR UPDATE SKIP LOCKED` 和短租约；网络请求不得持有数据库锁：

```text
claim event with short lease
  -> if runId missing: create_durable_run(saved route/body)
  -> persist compute_node_id + compute_run_id
  -> if cancel requested: POST /api/runs/cancel
  -> GET /api/runs/status
  -> queued/running: release and reschedule poll
  -> completed/failed/cancelled: terminal transaction
```

提交失败后重试必须复用已持久化的 route、body 和 `clientJobId`；这是跨 Outbox 至少一次投递的
`idempotency` 边界。网络请求/计算期间不得持有 PostgreSQL 行锁。取消（`cancel`）后继续轮询，
Compute 若已完成就保存 completed 事实，不能伪造成 cancelled。

`progressPercent` 是当前 adapter 从 kernel stage progress 取整后的 0..100。正式 UI 若需要阶段名、
ETA、实际 engine/scalar、`kernelReported` 和硬件执行证据，必须扩展生产 OpenAPI；不能从请求的
`engine` 猜测硬件。

## 6. 当前明确缺口

生产 OpenAPI 目前没有以下接口，服务后端不得假装已经存在：

- capabilities/health 的生产 `/api` 版本；
- manifest/SHA-256 获取；
- artifact 私有流式下载；
- status 中的 `hardwareExecution`、实际 engine/scalar、逃逸证书；
- Orbit Program/DSL 在生产 `/api` body 中的字段。

这些能力已经存在于另一套 `/compute/v1/*` 工具合同中，但当前 Platform `ComputeClient` 不调用它。
M3 资产摄取和付费 GPU 硬件验收前，必须先扩展协作者 OpenAPI 与 `ComputeClient`，再实现 C++
对应生产路由；不能让 Worker 私自混用两套 DTO。

## 7. 验收命令与结果

```bash
cmake -S backend -B runtime/build -DCMAKE_BUILD_TYPE=Release
cmake --build runtime/build -j2
ctest --test-dir runtime/build --output-on-failure
```

2026-07-24 当前结果：CTest 9/9；Compute HTTP pytest 86 个测试通过；其中测试会启动真实 C++
进程，并直接导入协作者的真实 `ComputeClient` 完成 preview、create、poll 和 PNG artifact DTO 解析。
测试位于 `backend/src/tests/compute_v1/test_platform_*.py`，每个测试可独立执行。

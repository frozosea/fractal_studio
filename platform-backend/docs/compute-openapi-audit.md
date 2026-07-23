# `compute-openapi.yaml` 与 `fractal_studio-master` 的审计

日期：2026-07-23。方法：静态比对 OpenAPI、`fractal_studio-master` 中的 HTTP dispatch、route 实现和 typed frontend client。未启动服务。

## 结论

OpenAPI 描述的 7 个路径均以相同 HTTP 方法存在。但该文档是**目标 private production contract**，不是当前 C++ API 的描述。文档本身明确指出 Compute 尚需实现 service authentication 与基于 `clientJobId` 的 idempotency（[`compute-openapi.yaml:5`](compute-openapi.yaml:5)）。

当前不能将文档作为 Platform M2/M7 的实际规范：必填字段、安全机制、错误和 durable job 的响应均与服务端不兼容。

| 路径 | 路径/方法存在 | 与 contract 一致 |
|---|---:|---:|
| `POST /api/map/render-inline` | 是 | 部分一致 |
| `POST /api/map/render` | 是 | 否 |
| `POST /api/video/export` | 是 | 否 |
| `POST /api/hs/mesh` | 是 | 否 |
| `POST /api/transition/mesh` | 是 | 否 |
| `GET /api/runs/status` | 是 | 否 |
| `POST /api/runs/cancel` | 是 | 部分一致 |

## 所有操作的关键差异

1. **未实现 Bearer auth。** OpenAPI 全局定义 `ComputeServiceKey`（[`compute-openapi.yaml:11`](compute-openapi.yaml:11)）；C++ server 仅从请求中提取 method/path/body，未解析 authorization header（[`http_server.cpp:78`](../../../fractal_studio-master/backend/src/core/http_server.cpp:78)）。CORS 只允许 `Content-Type`（[`http_server.cpp:65`](../../../fractal_studio-master/backend/src/core/http_server.cpp:65)）。因此规范中的 `401` 与 `403` 不可达。
2. **未使用 `clientJobId`。** 它是四个 durable operation 的必填字段，并承诺重复请求返回同一个 `runId`（[`compute-openapi.yaml:185`](compute-openapi.yaml:185)）。所有对应处理器均不读取此字段，而是总会通过 `runner.createRun(...)` 创建新 run，例如 map（[`routes_map.cpp:515`](../../../fractal_studio-master/backend/src/api/routes_map.cpp:515)）、video（[`routes_video.cpp:2353`](../../../fractal_studio-master/backend/src/api/routes_video.cpp:2353)）、HS（[`routes_mesh.cpp:205`](../../../fractal_studio-master/backend/src/api/routes_mesh.cpp:205)）和 transition mesh（[`routes_mesh.cpp:364`](../../../fractal_studio-master/backend/src/api/routes_mesh.cpp:364)）。重试会创建新任务。
3. **不存在统一 `Problem` 格式。** 文档要求 `{code,message,requestId}`（[`compute-openapi.yaml:267`](compute-openapi.yaml:267)）。server 对异常仅返回 `{"error":"..."}`（[`http_server.cpp:41`](../../../fractal_studio-master/backend/src/core/http_server.cpp:41)）；未分类异常转为 `500`（[`http_server.cpp:169`](../../../fractal_studio-master/backend/src/core/http_server.cpp:169)）。不会产生 `413`、`429`、`401` 或 `403`。无效 JSON 会变为 `{}`，而非 `400`（[`routes_common.hpp:75`](../../../fractal_studio-master/backend/src/api/routes_common.hpp:75)）。
4. **Schema 是 permissive，不是 strict。** 大多数 request 字段在 C++ 中有 default，并非必填（map：[`routes_map.cpp:258`](../../../fractal_studio-master/backend/src/api/routes_map.cpp:258)；HS：[`routes_mesh.cpp:180`](../../../fractal_studio-master/backend/src/api/routes_mesh.cpp:180)；transition：[`routes_mesh.cpp:339`](../../../fractal_studio-master/backend/src/api/routes_mesh.cpp:339)）。未知字段会被忽略。这与 OpenAPI 的 required 字段相冲突（[`compute-openapi.yaml:188`](compute-openapi.yaml:188)、[`compute-openapi.yaml:215`](compute-openapi.yaml:215)、[`compute-openapi.yaml:228`](compute-openapi.yaml:228)）。

## 按操作说明

### `POST /api/map/render-inline`

一致部分：返回 `application/octet-stream`、RGBA8；支持取消时的 `204`；会设置 `X-FSD-Width`、`X-FSD-Height`、`X-FSD-Pixel-Format`（[`routes_map.cpp:662`](../../../fractal_studio-master/backend/src/api/routes_map.cpp:662)）。

差异：

- 文档将 headers 的尺寸限制为 `1..1024`（[`compute-openapi.yaml:27`](compute-openapi.yaml:27)）；实际 `render-inline` 仅接受 `64..8192` 的 width/height（[`routes_map.cpp:302`](../../../fractal_studio-master/backend/src/api/routes_map.cpp:302)）。
- Contract 中 `MapRequest` 必填；server 会填入 default。还支持未文档化字段：`metric`、`smooth`、`preemptKey`、`preemptSeq`、`taskType`、`colorMode`、`cyclesPerOctave`、`rotationDeg`、高精度 `centerReStr`/`centerImStr` 与 transition 字段（[`routes_map.cpp:258`](../../../fractal_studio-master/backend/src/api/routes_map.cpp:258)）。
- OpenAPI 的 `stillExport: true` 不会被识别：server 只检查 `taskType == "still_export"`（[`routes_map.cpp:292`](../../../fractal_studio-master/backend/src/api/routes_map.cpp:292)）。

### `POST /api/map/render`

路径存在且会创建 PNG artifact，但 contract 不会按定义执行：

- `stillExport` 被忽略；应使用 `taskType: "still_export"`。`background` 仅在该 task type 下生效（[`routes_map.cpp:624`](../../../fractal_studio-master/backend/src/api/routes_map.cpp:624)）。因此，符合 OpenAPI 的 `{stillExport:true, background:true, clientJobId:...}` 会启动普通同步 map-run，而不是 queued export。
- `RunAccepted` 要求 `clientJobId`（[`compute-openapi.yaml:242`](compute-openapi.yaml:242)）；实际 queued response 包含 `runId`、`status`、`localExport`、尺寸、`requestId`、`effective`，但没有 `clientJobId`（[`routes_map.cpp:638`](../../../fractal_studio-master/backend/src/api/routes_map.cpp:638)）。同步 response 还包含 artifact URL、timing 与 effective parameters（[`routes_map.cpp:586`](../../../fractal_studio-master/backend/src/api/routes_map.cpp:586)）。
- Map limits：实际最小 `64`、最大 `8192`；OpenAPI 为 `1..4096`。

### `POST /api/video/export`

路径存在，默认异步：`background` default 为 `true`，返回 `queued`（[`routes_video.cpp:3298`](../../../fractal_studio-master/backend/src/api/routes_video.cpp:3298)）。但 `VideoRequest` schema 与实现不一致：

- OpenAPI 的 `durationSeconds`（[`compute-openapi.yaml:209`](compute-openapi.yaml:209)）不会被 server 读取。实际字段名为 `durationSec`；替代参数为 `depthOctaves` 和 `secondsPerOctave`（[`routes_video.cpp:343`](../../../fractal_studio-master/backend/src/api/routes_video.cpp:343)）。
- 实际 FPS 为 `1..120`，不是 `1..60`（[`routes_video.cpp:2264`](../../../fractal_studio-master/backend/src/api/routes_video.cpp:2264)）；尺寸为 `128..8192`，且面积不超过 7680×4320（[`routes_video.cpp:289`](../../../fractal_studio-master/backend/src/api/routes_video.cpp:289)）。
- 返回扩展 payload（`frameCount`、`durationSec`、`depthOctaves`、ln-map 参数、内存估计），没有 `clientJobId`（[`routes_video.cpp:3321`](../../../fractal_studio-master/backend/src/api/routes_video.cpp:3321)）。

### `POST /api/hs/mesh`

路径会创建 GLB 与 STL，但始终同步完成并返回 `completed`；response 含两个 artifact ID/URL 和 counts，而非 `RunAccepted`（[`routes_mesh.cpp:234`](../../../fractal_studio-master/backend/src/api/routes_mesh.cpp:234)）。

- 所有 input 都有 default：`centerRe=-0.75`、`scale=3`、`resolution=192`、`iterations=512`、`variant=mandelbrot`（[`routes_mesh.cpp:180`](../../../fractal_studio-master/backend/src/api/routes_mesh.cpp:180)）。
- 实际 `resolution` 上限为 4096，不是 1024（[`routes_mesh.cpp:199`](../../../fractal_studio-master/backend/src/api/routes_mesh.cpp:199)）。
- 支持但文档未描述：`metric`、`pairwiseCap`、`bailoutSq`；`clientJobId` 被忽略。

### `POST /api/transition/mesh`

路径为同步操作，返回 GLB/STL IDs 与 URLs、`vertexCount`、`triangleCount`、`fieldMs`、`mcMs`、实际 engine/scalar（[`routes_mesh.cpp:409`](../../../fractal_studio-master/backend/src/api/routes_mesh.cpp:409)）；不是 `RunAccepted`，也不使用 `clientJobId`。

- 所有主要 input 均有 default：centre `0`、`extent=2`、`iterations=256`、variants `mandelbrot`/`burning_ship`（[`routes_mesh.cpp:339`](../../../fractal_studio-master/backend/src/api/routes_mesh.cpp:339)）。
- 当 `resolution >= 512` 时，必须显式指定 `allowLargeVolume=true`；存在 memory/VRAM guard（[`routes_mesh.cpp:105`](../../../fractal_studio-master/backend/src/api/routes_mesh.cpp:105)）。这是未文档化的重要实际字段。
- 支持未文档化的 `bailoutSq`、`transitionVariants`、`transitionWeights`、`transitionLegs`；`scalarType` 被实际使用，但 frontend request type 中未列出。

### `GET /api/runs/status`

`runId` 参数一致。但 response 不兼容：实际字段是 `id`、`module`、`status`、`startedAt`、`finishedAt`、`outputDir`、对象 `progress`，以及 artifacts 数组中的 `name`、`kind`、`downloadUrl`、`contentUrl`、`localPath`（[`routes_runs.cpp:72`](../../../fractal_studio-master/backend/src/api/routes_runs.cpp:72)）。OpenAPI 期望 `clientJobId`、`progressPercent`、`error` 和 artifact 的 `purpose`、`mediaType`、`sizeBytes`（[`compute-openapi.yaml:249`](compute-openapi.yaml:249)）。

缺失或未知的 `runId` 会抛出内部异常（[`routes_runs.cpp:45`](../../../fractal_studio-master/backend/src/api/routes_runs.cpp:45)），HTTP layer 返回 `500`，而不是文档声明的 `404`。

### `POST /api/runs/cancel`

主要 body `{runId}` 和 response `{runId,status:"cancel_requested"}` 一致（[`routes_runs.cpp:122`](../../../fractal_studio-master/backend/src/api/routes_runs.cpp:122)）。差异：

- `additionalProperties: false` 未遵守：额外字段会被忽略。
- 实际还存在 `POST /api/runs/{runId}/cancel`（[`http_server.cpp:151`](../../../fractal_studio-master/backend/src/core/http_server.cpp:151)），文档中没有。
- 未知 run 会在 `requestCancel` 中抛出异常（[`job_runner.cpp:184`](../../../fractal_studio-master/backend/src/core/job_runner.cpp:184)），实际得到 `500`，而非 `404`。

## 未描述的实际 API

除 7 个路径外，contract 未描述 system（`/api/system/*`）、map preempt/field/ln、video preview/zoom/transition、HS field、transition voxels、special-points、benchmark、variants、runs list/active tasks 与 artifacts。当前 server 会 dispatch 它们（[`http_server.cpp:98`](../../../fractal_studio-master/backend/src/core/http_server.cpp:98)、[`http_server.cpp:112`](../../../fractal_studio-master/backend/src/core/http_server.cpp:112)、[`http_server.cpp:131`](../../../fractal_studio-master/backend/src/core/http_server.cpp:131)、[`http_server.cpp:143`](../../../fractal_studio-master/backend/src/core/http_server.cpp:143)、[`http_server.cpp:157`](../../../fractal_studio-master/backend/src/core/http_server.cpp:157)）。这不一定是 private-contract 的缺陷，但证明 YAML 仅覆盖未来的 Platform→Compute API，而非当前完整 C++ API。

## 建议

不要让 Platform worker 直接依赖当前 C++ response。先在 Compute 中实现 bearer auth、基于 `clientJobId` 的 lookup/reuse、strict validator、`Problem` envelope、声明的 404/413/429 semantics，以及稳定的 `RunAccepted`/`RunStatus`。之后用 integration tests 验证 contract。若目标是描述现有 local API，应单独维护一份 OpenAPI，而不是修改此 production contract。

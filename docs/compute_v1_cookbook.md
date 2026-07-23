# Compute v1 从零调用手册

这份手册回答三个实际问题：服务密钥从哪里来、每种 workload 是做什么的、怎样用真实 HTTP 请求公式/组合/transition。它面向本地联调和 Platform 服务后端开发；浏览器不得直接调用 Compute。

规范字段仍以 [HTTP 合同](compute_v1_contract.md) 和 [18 类任务参考](compute_v1_jobs.md) 为准。

## 1. Key 不是“申请”的

Compute v1 没有注册、登录或申请 Key 的接口。它使用一个由部署方生成的服务间共享密钥：

```text
C++ Compute 读取 FSD_COMPUTE_SERVICE_KEY
FastAPI API/Worker 读取 COMPUTE_SERVICE_KEY
两边的值必须完全相同
```

本地生成 256-bit 随机密钥：

```bash
openssl rand -hex 32
```

输出类似 `ef8a...` 的 64 位十六进制字符串。不要把真实值提交到 Git。

### 1.1 手动启动

终端 A，从仓库根目录启动 Compute：

```bash
export FSD_COMPUTE_SERVICE_KEY='替换成刚生成的64位字符串'
export FSD_ENABLE_LEGACY_API=0
export FSD_ENABLE_LEGACY_FORMULA_COMPILER=0
export FSD_STARTUP_BENCHMARK=off
export FSD_RENDERER_VERSION="$(git rev-parse --short HEAD)"
backend/build/fractal_studio_backend 18080
```

终端 B 配置测试请求：

```bash
export COMPUTE_BASE_URL='http://127.0.0.1:18080'
export COMPUTE_SERVICE_KEY='与终端A完全相同的64位字符串'
```

验证：

```bash
curl -sS "$COMPUTE_BASE_URL/compute/v1/health" | jq

curl -sS "$COMPUTE_BASE_URL/compute/v1/capabilities" \
  -H "Authorization: Bearer $COMPUTE_SERVICE_KEY" | jq
```

health 不需要 Key；capabilities 返回 401 说明两边密钥不一致或 Authorization 格式错误。

### 1.2 Docker Compose

仓库的 `docker-compose.dev.yml` 使用固定的 `development-compute-key`，只适合本机开发。生产部署应把两个位置同时替换为 Secret Manager/Kubernetes Secret/容器平台注入的同一个随机值：

```yaml
compute:
  environment:
    FSD_COMPUTE_SERVICE_KEY: ${COMPUTE_SERVICE_KEY}

api:
  environment:
    COMPUTE_SERVICE_KEY: ${COMPUTE_SERVICE_KEY}

worker:
  environment:
    COMPUTE_SERVICE_KEY: ${COMPUTE_SERVICE_KEY}
```

当前 v1 不支持同时接受新旧两把 Key。轮换时应协调更新 Compute 与 Worker 并滚动重启；不要设计成用户级 API Key，也不要保存到 PostgreSQL。

## 2. 先理解 workload

这里容易混淆两种叫法：

1. **job kind** 才是真正选择功能的 workload，例如 `map_image`、`zoom_video`、`transition_mesh`。
2. `benchmark` payload 里的 `workload` 只是性能结果的标签/缓存命名空间，例如 `interactive`、`batch`；它不选择公式，也不改变渲染算法。

### 2.1 按用途选择 kind

| 想得到什么 | kind | 同步/异步 | 典型用途 |
|---|---|---|---|
| 交互画布 RGBA | `map_image` preview | 同步 | 鼠标缩放后的快速预览 |
| PNG 主图 | `map_image` run | 异步 | 高质量图片导出 |
| 可重新染色的数值场 | `raw_field` | 同步 | 前端换 colormap 不重算轨道 |
| 深 zoom 对数条带 | `ln_map` | 异步 | 深 zoom 视频中间资产 |
| 深 zoom 视频 | `zoom_video` | 异步 | 围绕一个点连续放大 |
| 旧条带转视频 | `legacy_zoom_video` | 异步 | 迁移历史 ln-map，不用于新配方 |
| Zoom 起止帧预检 | `video_preview` | 同步 | 正式视频前确认目标点和色彩 |
| 两公式轴空间旋转/缩放视频 | `transition_video` | 异步 | Mandelbrot 到 Burning Ship 的升维切片动画 |
| Transition 起止帧 | `transition_video_preview` | 同步 | 正式 transition 视频前检查 |
| 2D 数值高度场 | `hs_field` | 异步 | 浏览器自行构造可调高度的 3D 地形 |
| 2D 高度场网格 | `hs_mesh` | 异步 | GLB/STL 导出 |
| Axis transition 等值面 | `transition_mesh` | 异步 | 两公式之间的三维连续体 |
| Axis transition 方块表面 | `transition_voxels` | 异步 | STL voxel 风格模型 |
| 特殊点完整枚举 | `special_points_enumerate` | 异步 | 小周期 center/Misiurewicz 数据集 |
| Viewport 特殊点搜索 | `special_points_search` | 异步 | 当前画面内寻找可缩放目标 |
| Newton 自动/seed/snap | `special_points_auto/seed/snap` | 同步 | 工具型点求解 |
| 节点硬件校准 | `benchmark` | 异步 | 测真实 CPU/SIMD/CUDA 路径吞吐 |

计算量不是由 `benchmark.workload` 决定，而主要由：

- 2D：`width × height × iterations`；
- HS：`resolution² × iterations`；
- transition volume：`resolution³ × iterations`；
- 视频：`width × height × frameCount × iterations`，另加 ln-map/编码；
- recurrence metric：还会受 `pairwiseCap²` 影响。

Platform 应根据产品 SKU 限制这些字段，不能让浏览器任意透传最大值。

## 3. 三种调用模式

### 3.1 同步 JSON preview

```bash
curl -sS "$COMPUTE_BASE_URL/compute/v1/previews" \
  -H "Authorization: Bearer $COMPUTE_SERVICE_KEY" \
  -H 'Content-Type: application/json' \
  -d '{
    "schemaVersion": 1,
    "kind": "raw_field",
    "payload": {
      "centerRe": -0.75,
      "centerIm": 0,
      "scale": 3,
      "width": 64,
      "height": 64,
      "iterations": 128,
      "metric": "escape"
    }
  }' | jq
```

### 3.2 同步 RGBA preview

`map_image` preview 返回 RGBA8 裸字节，不是 PNG：

```bash
curl -sS "$COMPUTE_BASE_URL/compute/v1/previews" \
  -H "Authorization: Bearer $COMPUTE_SERVICE_KEY" \
  -H 'Content-Type: application/json' \
  -D preview.headers \
  -o preview.rgba \
  -d '{
    "schemaVersion": 1,
    "kind": "map_image",
    "payload": {
      "centerRe": -0.75,
      "centerIm": 0,
      "scale": 3,
      "width": 256,
      "height": 256,
      "iterations": 256,
      "colorMap": "viridis"
    }
  }'

wc -c preview.rgba
```

256×256 的正确长度是 `262144` 字节。宽高和实际 engine/scalar 在 `preview.headers` 的 `X-FSD-*` 头中。

### 3.3 异步 run

所有持久任务都使用相同流程。这里创建 PNG：

```bash
CREATE_RESPONSE=$(curl -sS "$COMPUTE_BASE_URL/compute/v1/runs" \
  -H "Authorization: Bearer $COMPUTE_SERVICE_KEY" \
  -H 'Content-Type: application/json' \
  -d '{
    "schemaVersion": 1,
    "kind": "map_image",
    "idempotencyKey": "manual-map-0001",
    "payload": {
      "centerRe": -0.75,
      "centerIm": 0,
      "scale": 3,
      "width": 512,
      "height": 512,
      "iterations": 500,
      "engine": "auto",
      "scalarType": "auto",
      "colorMap": "inferno"
    }
  }')

echo "$CREATE_RESPONSE" | jq
export COMPUTE_RUN_ID=$(echo "$CREATE_RESPONSE" | jq -r '.data.computeRunId')
```

轮询：

```bash
curl -sS "$COMPUTE_BASE_URL/compute/v1/runs/$COMPUTE_RUN_ID" \
  -H "Authorization: Bearer $COMPUTE_SERVICE_KEY" | jq
```

重复执行直到 `.data.status` 为 `completed`、`failed` 或 `cancelled`。完成后读取 manifest：

```bash
curl -sS "$COMPUTE_BASE_URL/compute/v1/runs/$COMPUTE_RUN_ID/manifest" \
  -H "Authorization: Bearer $COMPUTE_SERVICE_KEY" | tee manifest.json | jq
```

下载第一个产物：

```bash
export ARTIFACT_ID=$(jq -r '.artifacts[0].artifactId' manifest.json)

curl -sS --get "$COMPUTE_BASE_URL/compute/v1/artifacts" \
  -H "Authorization: Bearer $COMPUTE_SERVICE_KEY" \
  --data-urlencode "artifactId=$ARTIFACT_ID" \
  -o artifact.bin

sha256sum artifact.bin
```

必须把 `sha256sum` 和 manifest 的 `sha256` 比较，不能只相信 HTTP 200。

取消：

```bash
curl -sS -X POST \
  "$COMPUTE_BASE_URL/compute/v1/runs/$COMPUTE_RUN_ID/cancel" \
  -H "Authorization: Bearer $COMPUTE_SERVICE_KEY" \
  -H 'Content-Type: application/json' \
  -d '{}' | jq
```

取消响应的 `accepted=true` 不是终态，仍要继续轮询。

## 4. 自定义公式到底是什么

DSL 的 `source` 定义**一次迭代输出**：

```text
z_(n+1) = source(z_n, c, n, parameters)
```

`n` 从 0 开始。参数平面默认 `z0=0`、`c=pixel`；Julia 模式默认 `z0=pixel`、`c=juliaRe+juliaIm*i`。

### 4.1 最简单的公式

`z*z+c` 等价于 Mandelbrot 的迭代步骤：

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

把它放进 map payload：

```bash
curl -sS "$COMPUTE_BASE_URL/compute/v1/runs" \
  -H "Authorization: Bearer $COMPUTE_SERVICE_KEY" \
  -H 'Content-Type: application/json' \
  -d '{
    "schemaVersion": 1,
    "kind": "map_image",
    "idempotencyKey": "dsl-cubic-0001",
    "payload": {
      "centerRe": 0,
      "centerIm": 0,
      "scale": 3,
      "width": 512,
      "height": 512,
      "iterations": 300,
      "orbitProgram": {
        "type": "formula",
        "formula": {
          "type": "dsl",
          "source": "pow(z,3)+c",
          "parameters": {}
        }
      }
    }
  }' | jq
```

### 4.2 带参数公式

`parameters` 必须是 object，不是数组：

```json
{
  "type": "formula",
  "formula": {
    "type": "dsl",
    "source": "z*z+c+a*sin(z)+shift",
    "parameters": {
      "a": 0.12,
      "shift": {"re": 0.01, "im": -0.02}
    }
  }
}
```

实数参数写 number；复数可写 `[re, im]` 或 `{"re":...,"im":...}`。参数名以字母开头，只含字母、数字、下划线，且不能使用保留名 `z/c/n/i/pi/e`。

### 4.3 语法表

| 类别 | 内容 |
|---|---|
| 变量 | `z`, `c`, `n` |
| 常量 | `i`, `pi`, `e` |
| 运算 | `+`, `-`, `*`, `/`, `^`, 一元正负号，括号 |
| 一元函数 | `sin`, `cos`, `tan`, `exp`, `log`, `sqrt`, `abs`, `conj`, `sinh`, `cosh`, `tanh`, `real`, `imag` |
| 二元函数 | `pow(a,b)` |

不支持赋值、条件、循环、递归、数组访问或用户自定义函数。DSL 出错返回 code 和从 0 开始的 `details.position`。

### 4.4 逃逸结果为什么不同

任意 DSL 默认 `escapeAnalysis.status=unverified`、`certifiedRadius=null`。这表示 Compute 不会拿一个猜测的有限阈值提前宣称数学逃逸；轨道会运行到迭代上限或数值非有限。它不是报错，而是“有限迭代轨道图”。

因此自定义公式通常比已认证的 `mandelbrot` 更慢。Platform UI 应展示证明状态，并对 DSL 使用更严格的尺寸/迭代配额。

## 5. 一步 Mandelbrot、一步 Burning Ship

这不是两个输出做图片插值，而是每个像素的同一条轨道按确定日程更新：

```text
n=0: Mandelbrot step
n=1: Burning Ship step
n=2: Mandelbrot step
n=3: Burning Ship step
...
```

请求中的 Orbit Program：

```json
{
  "type": "sequence",
  "repeat": true,
  "steps": [
    {
      "span": 1,
      "program": {
        "type": "formula",
        "formula": {"type": "builtin", "id": "mandelbrot"}
      }
    },
    {
      "span": 1,
      "program": {
        "type": "formula",
        "formula": {"type": "builtin", "id": "burning_ship"}
      }
    }
  ]
}
```

可用于 `map_image`, `raw_field`, `ln_map`, `zoom_video`, `video_preview`, `hs_field`, `hs_mesh`。例如把它复制到第 3.3 节 map payload 的 `orbitProgram` 即可。`span:3` 表示该公式连续执行三次再切换；`repeat` 在 v1 必须为 true。

当前不支持按 50% 概率随机选公式，也不支持把两个公式的复数输出线性相加。未来的 `weighted_schedule` 和 `output_blend` 是不同数学系统，在 capabilities 中仍为 false。

## 6. Transition 到底是什么

现有 transition 是 axis lift：把两个 quadratic/folded 动力系统耦合到更高维空间，再观察二维切片、三维等值面或动画。它不是 `A(z)*(1-t)+B(z)*t`，也不接受普通 `orbitProgram`。

### 6.1 Rotation video

从 Mandelbrot 平面旋转到 Burning Ship 平面：

```bash
curl -sS "$COMPUTE_BASE_URL/compute/v1/runs" \
  -H "Authorization: Bearer $COMPUTE_SERVICE_KEY" \
  -H 'Content-Type: application/json' \
  -d '{
    "schemaVersion": 1,
    "kind": "transition_video",
    "idempotencyKey": "transition-rotation-0001",
    "payload": {
      "animationMode": "rotation",
      "transitionFrom": "mandelbrot",
      "transitionTo": "burning_ship",
      "centerRe": -0.75,
      "centerIm": 0,
      "scale": 3,
      "thetaStartDeg": 0,
      "thetaEndDeg": 90,
      "width": 512,
      "height": 512,
      "iterations": 300,
      "fps": 30,
      "durationSec": 4,
      "engine": "auto",
      "scalarType": "auto"
    }
  }' | jq
```

`theta=0°` 是 from 平面，`theta=90°` 是 to 平面。中间帧是 axis 动力系统切片，不是两张图淡入淡出。

### 6.2 固定 transition 切片上的 zoom video

```json
{
  "schemaVersion": 1,
  "kind": "transition_video",
  "idempotencyKey": "transition-zoom-0001",
  "payload": {
    "animationMode": "zoom",
    "transitionFrom": "mandelbrot",
    "transitionTo": "burning_ship",
    "thetaDeg": 45,
    "centerRe": -0.743643887037151,
    "centerIm": 0.13182590420533,
    "width": 512,
    "height": 512,
    "iterations": 500,
    "depthOctaves": 8,
    "secondsPerOctave": 0.5,
    "fps": 30
  }
}
```

把该 JSON 作为 `/compute/v1/runs` body 发送。这里角度不变化，只在 45° slice 内连续 zoom。

### 6.3 Transition mesh

```bash
curl -sS "$COMPUTE_BASE_URL/compute/v1/runs" \
  -H "Authorization: Bearer $COMPUTE_SERVICE_KEY" \
  -H 'Content-Type: application/json' \
  -d '{
    "schemaVersion": 1,
    "kind": "transition_mesh",
    "idempotencyKey": "transition-mesh-0001",
    "payload": {
      "transitionFrom": "mandelbrot",
      "transitionTo": "burning_ship",
      "centerX": 0,
      "centerY": 0,
      "centerZ": 0,
      "extent": 2,
      "resolution": 128,
      "iterations": 128,
      "iso": 0.5,
      "engine": "auto",
      "scalarType": "fp32"
    }
  }' | jq
```

完成 manifest 中应有 GLB 和 STL。`resolution³` 增长很快：128³ 约 210 万 voxel，256³ 约 1678 万，512³ 约 1.34 亿；Platform 不应把 512 作为普通用户默认值。

### 6.4 Multi-axis 的当前边界

`transition_mesh`/`transition_voxels` 可以使用 `transitionLegs` 描述多个轴：

```json
{
  "transitionLegs": [
    {"variant": "mandelbrot", "weight": 1.0},
    {"variant": "burning_ship", "weight": 1.0},
    {"variant": "tricorn", "weight": 0.5}
  ]
}
```

这些 weight 属于 axis 耦合参数，不是 output blend 权重。`transition_video` 的 multi 动画语义尚未开放，发送 `transitionMode=multi` 或 `transitionVariants` 会明确失败。

Axis transition 当前允许的 variant ID 是：`mandelbrot`, `tricorn`, `burning_ship`, `celtic`, `heart`, `buffalo`, `perp_buffalo`, `celtic_ship`, `mandelceltic`, `perp_ship`。`sin_z` 等 transcendental 和 DSL 不能进入 axis transition。

## 7. 其他常用请求

### 7.1 HS mesh 使用 Orbit sequence

```json
{
  "schemaVersion": 1,
  "kind": "hs_mesh",
  "idempotencyKey": "hs-sequence-0001",
  "payload": {
    "centerRe": -0.75,
    "centerIm": 0,
    "scale": 3,
    "resolution": 256,
    "iterations": 256,
    "metric": "min_abs",
    "heightScale": 0.6,
    "heightClamp": 2.0,
    "orbitProgram": {
      "type": "sequence",
      "repeat": true,
      "steps": [
        {"span": 1, "program": {"type": "formula", "formula": {"type": "builtin", "id": "mandelbrot"}}},
        {"span": 1, "program": {"type": "formula", "formula": {"type": "builtin", "id": "burning_ship"}}}
      ]
    }
  }
}
```

### 7.2 Viewport 特殊点搜索

```json
{
  "schemaVersion": 1,
  "kind": "special_points_search",
  "idempotencyKey": "points-search-0001",
  "payload": {
    "kind": "center",
    "periodMin": 1,
    "periodMax": 8,
    "seedBudget": 2000,
    "viewport": {
      "centerRe": -0.75,
      "centerIm": 0,
      "scale": 3,
      "rotationDeg": 0,
      "width": 1200,
      "height": 800
    }
  }
}
```

注意外层 `kind=special_points_search` 是任务类型，内层 `payload.kind=center` 是点类型。

### 7.3 Benchmark

```json
{
  "schemaVersion": 1,
  "kind": "benchmark",
  "idempotencyKey": "benchmark-node-0001",
  "payload": {
    "width": 512,
    "height": 512,
    "iterations": 2000,
    "samples": 3,
    "warmup": 1,
    "replaceCache": true,
    "workload": "interactive"
  }
}
```

`workload` 只把这组结果标为 interactive。Compute 仍会测试当前节点可用的 OpenMP/SIMD/CUDA candidates；实际使用路径看 manifest 的 `hardwareExecution.paths[]`。

## 8. FastAPI 应该怎样暴露这些能力

浏览器不应该看到上述 Compute URL 或 Key。推荐流程：

```text
browser recipe request
  -> FastAPI 验证用户、配额、kind 与 payload
  -> 保存不可变 recipe snapshot
  -> render_jobs + outbox_events 同一事务
  -> Worker 用服务 Key 调用 Compute
  -> Worker 轮询、验证硬件和 SHA-256
  -> 对象存储
  -> FastAPI 返回平台 job/asset
```

Platform 不应提供“任意 Compute JSON 代理”。至少要限制：允许的 kind、尺寸、迭代、视频时长、volume resolution、DSL 长度、Orbit 节点数、请求硬件 SKU 和产物类型。

现有内部客户端位于 `platform-backend/app/infrastructure/compute/compute_client.py`。部署环境配置：

```bash
export COMPUTE_BASE_URL='http://compute:18080'
export COMPUTE_SERVICE_KEY='与Compute相同的服务密钥'
export COMPUTE_NODE_ID='compose-compute-1'
```

完整 Worker 数据库和恢复流程见 [Platform 对接指南](platform_compute_integration.md)。

## 9. 常见错误

| 现象 | 原因 |
|---|---|
| capabilities 返回 401 | Key 缺失、不一致或没写 `Bearer ` 前缀 |
| map preview 无法当 PNG 打开 | 它是 RGBA8 裸字节 |
| 自定义公式 parameters 报错 | `parameters` 必须是 object；复数必须 `[re,im]` 或 `{re,im}` |
| DSL 图看起来“没逃逸” | DSL 默认无有限逃逸证书，会严格跑到上限/数值发散 |
| Orbit + transition 返回 422 | 两者是不同顶层数学系统，不能嵌套 |
| transition video multi 失败 | multi 动画语义尚未开放；mesh/voxels 的 axis multi 可用 |
| 请求 CUDA 但 actual 是 CPU | 节点不可用或发生回退；看 `hardwareExecution`，不要只看请求值 |
| POST /runs 超时后出现重复任务 | 重试时换了 idempotencyKey；同一平台任务必须永久复用同一 key |
| cancel accepted 但最后 completed | 协作取消与完成竞争，这是合法终态 |

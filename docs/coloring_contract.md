# Coloring / 染色接口与自定义调色板合同

最后更新：2026-07-24

染色是数值场之后的展示层，不改变 Orbit Program、逃逸证书或迭代结果。架构上允许相同 field
重复染色；当前 map image 接口仍一次完成计算与染色，`raw_field` 消费者可以在平台侧缓存数值场后重染色。

## 1. 当前内置接口

二维 map/preview/PNG 使用以下字段：

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `colorMap` | string | `classic_cos` | 11 个内置调色板 ID。 |
| `smooth` | boolean | `false` | escape 使用连续迭代值 `mu`；非 escape 使用对数场映射。 |
| `colorMode` | string | `direct` | `direct`、`eq_full`、`eq_center`。 |
| `cyclesPerOctave` | number | `1.0` | equalized 模式色带密度，范围 `(0,64]`。 |

内置 `colorMap`：

```text
classic_cos mod17 hsv_wheel tri765 grayscale hs_rainbow
inferno viridis twilight ember_blue spectral1530
```

ln-map 另外使用 `lnMapColorMode`：`escape`、`hist_eq`、`row_eq`、`log_lift`、`bands`、
`frontier`。它决定数值如何映射到色表，不是第二个调色板字段。

未知内置 ID 在生产合同中必须返回结构化 400/422，不能静默回退 `classic_cos`。

## 2. 自定义 `colorProgram` v1

第一版采用安全、声明式 gradient，而不是接受任意 C++、GLSL 或脚本：

```json
{
  "schemaVersion": 1,
  "type": "gradient",
  "interpolation": "rgb",
  "wrap": "repeat",
  "cycles": 2.0,
  "phase": 0.125,
  "interiorColor": "#050816",
  "invalidColor": "#ff00ff",
  "stops": [
    {"at": 0.0, "color": "#050816"},
    {"at": 0.35, "color": "#00b8a9"},
    {"at": 0.72, "color": "#ff9f43"},
    {"at": 1.0, "color": "#fff4d6"}
  ]
}
```

限制和语义：

- `schemaVersion=1`、`type=gradient`、`interpolation=rgb` 固定。
- 2..16 个 stop；`at` 必须从 0 到 1 严格递增，首尾必须正好为 0/1。
- 颜色只接受 `#RRGGBB`；输出保持不透明 RGB。
- `wrap=clamp|repeat|mirror`。
- `cycles` 为 `(0,256]`，`phase` 为有限数，先做 `t * cycles + phase` 再 wrap。
- `interiorColor` 是未逃逸像素颜色；`invalidColor` 用于非有限 field，默认分别为白/品红。
- escape + `smooth=false` 的输入 `t=(iter+1)/(maxIterations+2)`。
- escape + `smooth=true` 的输入 `t=mu/32`，其中 `mu=iter+1-log2(log2(abs(z)^2))`。
- 非 escape metric 使用现有 `raw/bailout` 归一化值。
- `colorProgram` 与 `colorMap` 二选一；同时发送返回错误。
- v1 仅支持 `colorMode=direct`。equalized、ln-map、视频和 WebGL 本地重染色要在各自实现支持后才能启用。

资源限制保证每像素最多扫描 16 个 stop；不得包含循环、函数、纹理、文件或动态 shader。

## 3. 完整请求示例

生产 `/api` adapter 已接受 `colorProgram`。注意：协作者当前 OpenAPI/mapper 尚未声明和转发该字段，
Platform 公共 API 接通前应先完成第 5 节任务。直接验证 C++ preview：

```bash
curl -sS "$COMPUTE_BASE_URL/api/map/render-inline" \
  -H "Authorization: Bearer $COMPUTE_SERVICE_KEY" \
  -H 'Content-Type: application/json' \
  --data-binary @- > preview.rgba <<'JSON'
{
  "requestId": "11111111-1111-4111-8111-111111111111",
  "width": 64,
  "height": 64,
  "iterations": 128,
  "variant": "mandelbrot",
  "centerRe": -0.75,
  "centerIm": 0.0,
  "scale": 3.0,
  "engine": "cpu",
  "scalarType": "double",
  "colorMode": "direct",
  "colorProgram": {
    "schemaVersion": 1,
    "type": "gradient",
    "interpolation": "rgb",
    "wrap": "repeat",
    "cycles": 2.0,
    "phase": 0.125,
    "interiorColor": "#050816",
    "invalidColor": "#ff00ff",
    "stops": [
      {"at": 0.0, "color": "#050816"},
      {"at": 0.5, "color": "#00b8a9"},
      {"at": 1.0, "color": "#fff4d6"}
    ]
  }
}
JSON
```

成功响应是 `64*64*4=16384` 字节裸 RGBA8，并通过 `X-FSD-Width/Height/Pixel-Format`
描述格式。异步 PNG 使用相同 map 字段请求 `POST /api/map/render`，再增加：

```json
{
  "clientJobId": "22222222-2222-4222-8222-222222222222",
  "stillExport": true,
  "background": true
}
```

这里是“增加到完整请求的字段”，不能把该三字段片段单独发送。创建后按
`GET /api/runs/status?runId=...` 轮询并读取 PNG artifact；完整生命周期见
[Platform–Compute 对接指南](platform_compute_integration.md)。

## 4. 首批能力矩阵

| Pipeline | built-in `colorMap` | custom `colorProgram` v1 |
|---|:---:|:---:|
| `/api/map/render-inline` | yes | yes |
| `/api/map/render` PNG | yes | yes |
| `/compute/v1` map preview/run | yes | yes |
| 2D axis transition slice 经 map route | yes | yes |
| raw field 数值输出 | n/a | n/a；由消费者重染色 |
| interactive field snapshot / WebGL | yes | pending |
| ln-map / zoom video / transition video | yes | pending，必须显式拒绝 |
| HS/transition mesh | n/a | n/a；材质属于资产展示层 |

## 5. Platform 与前端任务

当前协作者生产 OpenAPI 只声明 `colorMap: string`，mapper 也只转发内置 ID。要把自定义染色做成
商业配方，还需要：

### 服务后端

- 在 Recipe canonical schema 增加版本化 `colorProgram`，与 `colorMap` 做互斥校验。
- 规范化 stop 数字/颜色大小写并参与不可变 recipe hash、发布快照和商品版本。
- 扩展 `compute-openapi.yaml` 的 map request；mapper 原样转发规范化对象。
- 按 Compute capability 拒绝视频/ln-map 等未支持组合，不得删除字段后回退内置色表。

### 前端

- 增加 stop 编辑器、颜色选择、拖动排序、wrap/cycles/phase 和 interior color 控件。
- 保存的是声明式 `colorProgram`，不能保存/执行用户 GLSL。
- WebGL shader 增加最多 16 stops 的 uniform 路径，实现后才能本地无重算预览；此前通过 Compute preview。
- UI 明确显示“该输出暂不支持自定义染色”，不能悄悄换回 `classic_cos`。

如果后续需要的不只是自定义色带，而是对 `iteration/mu/raw field/orbit class` 做自定义映射，新增
`colorProgram` v2 的带类型声明式节点和指令预算；仍不得把用户 GLSL、Python 或动态原生代码交给
Compute 进程执行。

自定义调色板的命名、收藏、共享和市场存档属于 Platform 数据；C++ 只消费不可变 program。

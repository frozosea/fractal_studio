# Node Benchmark / 计算节点性能基准

每个计算节点在 HTTP 监听**之前**运行启动校准（`FSD_STARTUP_BENCHMARK`，
默认 `quick`：interactive 256x256x1000 + batch 512x512x2000 两个 workload，
warmup 1 + 2 次采样），结果写入进程内 benchmark cache，并由
`/compute/v1/capabilities` 的 `hardware.benchmark` 随注册/探活上报给
Gateway（`capabilities_json` 全量存储）。

## 如何测试

### 1. 启动校准 + capabilities（最常用）

```bash
cd /home/fractal-studio/fractal_studio
set -a && . ./.env && set +a
FSD_STARTUP_BENCHMARK=quick backend/build/fractal_studio_backend 18099 \
  > /tmp/backend.log 2>&1 &
# 日志出现 "calibration completed in ... ms; scheduler reference is ready"
# 后，另开终端：
KEY=$(grep FSD_COMPUTE_SERVICE_KEY .env | cut -d= -f2- | tr -d '"')
curl -s -H "Authorization: Bearer $KEY" http://127.0.0.1:18099/compute/v1/capabilities \
  | python3 -m json.tool | head -80
```

预期：`hardware.benchmark` 数组包含每个可用 engine/scalar 的
`mpixPerSec` / `elapsedMs` / `sampleCount` / `available`；CUDA 构建的节点
会带 `cuda/*` 行（RTX 4090 参考：cuda/fp32 ≈ 370-460 Mpix/s，
cuda/fp64 ≈ 35-50 Mpix/s）。

### 2. 手动按需 benchmark（任意 workload）

```bash
KEY=$(grep FSD_COMPUTE_SERVICE_KEY .env | cut -d= -f2- | tr -d '"')
curl -s -X POST -H "Authorization: Bearer $KEY" -H "Content-Type: application/json" \
  http://127.0.0.1:18099/api/benchmark \
  -d '{"width":1024,"height":1024,"iterations":500,"warmup":1,"samples":2,"workload":"my-test"}'
```

限制：dimension 64-2048、总像素 ≤ 2048²、workUnits ≤ 8e9、iterations ≤ 100000、
samples ≤ 7、warmup ≤ 3。结果按 engine/scalar 返回 `mpixPerSec`，
可用路径 `available=true`。

### 3. Gateway 注册链路（节点 → gateway → 数据库）

Gateway 的 `probe_node`（注册/激活时）拉 `/compute/v1/capabilities` 并写入
`compute_nodes.capabilities_json` / `capabilities_at`，之后 `probe_all`
按 `node_capabilities_refresh_seconds`（默认 60s）周期刷新。验证：

```bash
docker compose -f docker-compose.dev.yml exec postgres psql -U fractal \
  -c "SELECT node_key, capabilities_at, capabilities_json->'hardware'->'benchmark'->0
       FROM compute_nodes WHERE state='active' ORDER BY capabilities_at DESC;"
```

### 4. 关闭校准（可选）

`FSD_STARTUP_BENCHMARK=off`（默认是 quick；生产 compose 已显式 quick）。

## 注意

- 首次启动 CUDA 校准可能慢（kernel 初始化/编译，本机曾见 16s），之后 < 1s。
- 关闭校准的节点没有 benchmark 数据，调度退化为静态能力。
- 本地裸构建默认 **没有 CUDA**（nvcc 不在 PATH）；用
  `-DCMAKE_CUDA_COMPILER=/usr/local/cuda-13.2/bin/nvcc` 重新配置。
  生产 Docker 构建自带 CUDA（BuildKit cuda-toolkit context）。

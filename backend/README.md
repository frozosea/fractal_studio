# Fractal Studio Compute Backend

`backend/` 是私有 C++ Compute 服务。它负责分形数学、CPU/GPU 执行、Compute run 生命周期和临时产物；用户、订单、配额、资产与市场数据属于 `platform-backend/`。

第一次接入先看 [Compute v1 从零调用手册](../docs/compute_v1_cookbook.md)，它包含 Key、workload、curl、DSL/sequence、transition 和自定义染色示例。规范协议见 [Compute v1 Contract](../docs/compute_v1_contract.md)，逐任务字段和产物见 [Compute v1 Jobs](../docs/compute_v1_jobs.md)，染色 schema 与支持矩阵见 [Coloring Contract](../docs/coloring_contract.md)，Worker 实现流程见 [Platform–Compute Integration](../docs/platform_compute_integration.md)。商业化实施状态见 [Commercialization Implementation](../docs/commercialization_implementation.md)。

## Build

Debian/Ubuntu 需要 CMake、C++20、OpenCV、SQLite、OpenSSL、OpenMP、MPFR/GMP 和 FFmpeg。CUDA 为可选能力，CMake 发现 `nvcc` 时自动编译 GPU kernel。

```bash
cmake -S backend -B backend/build -DCMAKE_BUILD_TYPE=Release
cmake --build backend/build -j2
```

启动服务：

```bash
export FSD_COMPUTE_SERVICE_KEY='replace-with-a-random-service-secret'
export FSD_ENABLE_LEGACY_API=0
export FSD_ENABLE_LEGACY_FORMULA_COMPILER=0
export FSD_STARTUP_BENCHMARK=off
backend/build/fractal_studio_backend 18080
```

进程工作目录必须是仓库根目录，运行数据库和产物写入 `runtime/`。`GET /compute/v1/health` 不鉴权；其余 `/compute/v1/*` 以及 Platform 使用的生产 `/api/*` 合同必须携带 `Authorization: Bearer <service-key>`。

本地同时启动旧前端时直接运行仓库根目录 `./dev.sh`。脚本会为 C++ 与 Vite 生成并注入同一个临时密钥，不落盘、不打印。`VITE_COMPUTE_SERVICE_KEY` 只用于本地迁移，禁止进入商业前端构建。

## Configuration

| Variable | Commercial value | Meaning |
|---|---|---|
| `FSD_COMPUTE_SERVICE_KEY` | required secret | Compute 私有服务间 Bearer 密钥；未设置时所有受保护路由拒绝访问。 |
| `FSD_ENABLE_LEGACY_API` | `0` | 是否开放浏览器历史 `/api/*`。开发迁移期可设为 `1`。 |
| `FSD_ENABLE_LEGACY_FORMULA_COMPILER` | `0` | 旧 `g++ + dlopen` 开关；只有 legacy API 也开启时才可能启用。商业环境必须为 `0`。 |
| `FSD_RENDERER_VERSION` | image/git version | 写入 health、capabilities 与 manifest 的渲染器版本。 |
| `FSD_STARTUP_BENCHMARK` | `off` or `quick` | 启动校准；容器快速启动通常使用 `off`，再提交异步 benchmark run。 |
| `FSD_RENDER_THREADS` | deployment-specific | OpenMP 计算线程上限。 |
| `FSD_THERMAL_FRIENDLY` | deployment-specific | 降低持续 CPU 压力的运行模式。 |

生产环境还应满足：Compute 只监听私网/安全组，只有 Platform Worker 持有服务密钥，运行卷不与浏览器共享，API 与 Worker 使用不同凭据。

## Tests

安装测试依赖并配置时指定解释器，可保证 CTest 注册真实 HTTP 合同测试：

```bash
python3 -m venv runtime/compute-test-venv
runtime/compute-test-venv/bin/pip install -r backend/requirements-test.txt
cmake -S backend -B backend/build -DCMAKE_BUILD_TYPE=Release \
  -DFSD_PYTEST_PYTHON="$PWD/runtime/compute-test-venv/bin/python"
cmake --build backend/build -j2
ctest --test-dir backend/build --output-on-failure
```

只运行 Compute v1 HTTP 合同：

```bash
runtime/compute-test-venv/bin/python -m pytest -q \
  backend/src/tests/compute_v1 \
  --backend-binary=backend/build/fractal_studio_backend \
  --studio-root=.
```

测试 fixture 会启动真实后端进程并发送 HTTP 请求。每个测试独立创建 run 和 artifact，不依赖前序测试。硬件验收读取 kernel 完成点返回的实际 engine/scalar；请求值不算执行证据。

## Mathematical safety

- 自定义公式通过 Pratt parser、带类型 AST 和定长栈字节码执行，不允许循环、递归、赋值、文件、网络或任意原生调用。
- Orbit Program 当前支持 `formula` 和周期 `sequence`；Mandelbrot/Burning Ship 可以按 `span` 确定性轮换。
- `axis_transition`/`axis_multi` 保持既有升维语义，不会转换为 output blend。
- 只有证明器成功时 `certifiedRadius` 才是有限值。未认证轨道不会因有限模长提前标记 escaped，只会跑满迭代或标记数值发散。
- `weighted_schedule`、`output_blend` 和参数曲线在 capabilities 中明确为未启用，不会静默回退。

## Runtime and recovery

Compute 使用 `runtime/db/fractal_studio.sqlite` 保存本机 run 状态，产物位于 `runtime/runs/`。它们不是商业事实来源；Platform Worker 校验 manifest 和 SHA-256 后再把商业资产上传对象存储。

进程重启时 JobRunner 会协调本地未完成状态。Platform 仍须把提交视为至少一次投递，并使用稳定 `idempotencyKey`；相同 key 会返回相同 Compute run。

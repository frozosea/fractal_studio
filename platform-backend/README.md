# Fractal Studio Platform Backend

商业平台的模块化 FastAPI 单体。浏览器最终只访问该服务；C++ Compute 通过私网 `/compute/v1/*` 契约调用。

Compute 服务 Key 不是在线申请的用户 Key。部署方生成随机共享密钥并同时注入 C++ 的 `FSD_COMPUTE_SERVICE_KEY` 与本服务 API/Worker 的 `COMPUTE_SERVICE_KEY`。从零配置和各类请求示例见 [Compute v1 调用手册](../docs/compute_v1_cookbook.md)。

当前里程碑仅包含 M2/M7 架构底座：开发/测试预览、持久渲染任务、PostgreSQL Outbox、Compute 轮询/取消和 manifest 校验。生产环境会在 M1 会话完成前强制关闭这些临时 Studio 路由。

## Local stack

从仓库根目录启动：

```bash
docker compose -f docker-compose.dev.yml up --build
```

服务：

- Platform API: `http://127.0.0.1:8000`
- private Compute development port: `http://127.0.0.1:18080`
- PostgreSQL: `127.0.0.1:5432`
- Redis: `127.0.0.1:6379`
- MinIO API/console: `127.0.0.1:9000` / `9001`

开发预览：

```bash
curl -X POST http://127.0.0.1:8000/v1/studio/preview \
  -H 'Content-Type: application/json' \
  -d '{"kind":"map_image","payload":{"width":64,"height":64,"iterations":64}}' \
  --output preview.rgba
```

创建任务需要 `Idempotency-Key`：

```bash
curl -X POST http://127.0.0.1:8000/v1/render-jobs \
  -H 'Content-Type: application/json' \
  -H 'Idempotency-Key: example-map-0001' \
  -d '{"kind":"map_image","payload":{"width":256,"height":256,"iterations":256}}'
```

## Production guard

`APP_ENV=production` 时 `FOUNDATION_ROUTES_ENABLED` 必须为 false，否则进程拒绝启动。生产 Compute 必须配置非空 `COMPUTE_SERVICE_KEY`，且 Compute 端必须使用相同的 `FSD_COMPUTE_SERVICE_KEY`。

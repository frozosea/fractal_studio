# Fractal Studio 支付系统深度分析

> 分析日期：2026-07-28 | 源码基线：`master`

---

## 一、四层架构划分

```
┌──────────────────────────────────────────────────────────────────────┐
│  L1  前端展示层 (Next.js / React)                                     │
│       explore/page.tsx  →  浏览挂牌 + 支付宝支付按钮                   │
│       purchases/page.tsx →  已购订单列表 + 资产下载                    │
│       platform.ts       →  API 客户端 (checkout / order / purchases)  │
├──────────────────────────────────────────────────────────────────────┤
│  L2  后端业务层 (FastAPI)                                             │
│       commerce/router.py    →  HTTP 端点 (checkout / order / webhook) │
│       commerce/service.py   →  下单编排 + 通知处理 + 对账 + 冲正      │
│       commerce/models.py    →  Pydantic 视图 (无秘密字段)             │
│       commerce/repository.py →  PostgreSQL 原始 SQL 持久化             │
│       finance/sale_ledger_writer.py → 不可变账本写入器                 │
│       outbox/worker.py      →  异步对账调度 + 指数退避重试             │
├──────────────────────────────────────────────────────────────────────┤
│  L3  支付接口层                                                       │
│       infrastructure/alipay/payment_gateway.py                        │
│         → RSA2 签名 · PKCS1v15+SHA256                                 │
│         → alipay.trade.page.pay (桌面) / alipay.trade.wap.pay (移动)  │
│         → alipay.trade.query / alipay.trade.close                     │
│         → 异步通知验签 + 网关响应验签                                   │
│       tests/e2e/alipay_stub.py                                       │
│         → 开发/测试 RSA2 存根 · 实时生成 2048-bit 密钥对               │
├──────────────────────────────────────────────────────────────────────┤
│  L4  数据库层 (PostgreSQL + Redis)                                    │
│       orders · order_items · payment_attempts                         │
│       payment_notifications · refund_reversals                       │
│       entitlements · ledger_entries · creator_balances               │
│       outbox_events (异步对账) · idempotency_records (幂等)           │
│       Redis: 登录/注册限流 (auth rate limiter)                        │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 二、支付宝对接完整逻辑

### 2.1 密钥体系

```
┌─────────────────────────────────────────────────────────────────┐
│  生产环境                    │  开发/Stub 环境                    │
├─────────────────────────────┼──────────────────────────────────┤
│  ALIPAY_APP_ID              │  alipay_stub_mode=true            │
│  ALIPAY_SELLER_ID           │  APP_ID → "dev-stub"              │
│  ALIPAY_PRIVATE_KEY_PATH    │  SELLER_ID → "dev-stub"           │
│    → PEM 文件读取            │  签名 → "dev-stub-" + SHA256      │
│  ALIPAY_PUBLIC_KEY_PATH     │  验签 → alipay_stub_public_key_url│
│    → PEM 文件读取            │    (stub 运行时生成 2048-bit RSA)  │
│  ALIPAY_NOTIFY_URL (HTTPS)  │  notify_url → stub 内网地址        │
│  ALIPAY_RETURN_URL (HTTPS)  │  return_url → stub 内网地址        │
│  ALIPAY_GATEWAY_URL         │  gateway → stub /gateway.do        │
│    → openapi.alipay.com     │                                    │
└─────────────────────────────────────────────────────────────────┘
```

**密钥加载流程（`payment_gateway.py:153-181`）：**

```
start_payment() / verify_notification() / query_trade()
  └─ _provider_public_key()
       ├─ if alipay_stub_mode:
       │    HTTP GET alipay_stub_public_key_url → 获取 stub 公钥 PEM
       └─ else:
            读取 ALIPAY_PUBLIC_KEY_PATH 磁盘文件 → PEM
       → 自动补全 BEGIN/END 标记 (_pem_public_key)
       → serialization.load_pem_public_key() → cryptography RSA 对象
       → 缓存到 self._public_key (避免反复 IO)

_sign()（生产模式）:
  读取 ALIPAY_PRIVATE_KEY_PATH → load_pem_private_key(password=None)
  → key.sign(content, PKCS1v15(), SHA256()) → base64

_sign()（stub 模式）:
  "dev-stub-" + SHA256(canonical_string)  // 确定性假签名
```

### 2.2 支付下单接口

**调用链：**

```
frontend/platform.ts
  platform.commerce.checkout(listing)
    → POST /v1/checkout  { listingId, licenceOfferId, channel }
      → commerce/router.py: checkout()
        → CheckoutService.checkout()
          ├─ MarketplaceService.find_published_offer()  // 获取挂牌快照
          ├─ 校验: currency=CNY, price∈[0.01, alipay_max_total_amount]
          ├─ idempotency_service.claim()  // 幂等锁
          ├─ repository.find_active_order_for_listing()  // 防重复下单
          ├─ _split(price) → creator_amount + platform_fee_amount
          │    platform_fee_bps=2000 (20%)
          ├─ repository.create_pending_order()
          │    → INSERT orders (pending_payment)
          │    → INSERT order_items (含 listing/licence 快照)
          │    → INSERT payment_attempts (created)
          ├─ AlipayPaymentGateway.start_payment()
          │    ├─ 构造 biz_content: { out_trade_no, product_code, total_amount,
          │    │    subject, seller_id }
          │    ├─ 生产模式: _sign(canonical_string) → RSA2 签名
          │    ├─ Stub 模式: "dev-stub-" + SHA256
          │    └─ 返回 PaymentFormDescriptor(action, method, fields)
          ├─ TransactionalOutboxService.append("payment.reconcile.v1")
          │    available_at = now + payment_reconcile_delay_seconds
          └─ idempotency_service.complete()  // 保存响应
            → 返回 { order, paymentAttempt, alipayForm }
```

**支付宝表单字段（生产环境）：**

| 字段 | 值 | 说明 |
|---|---|---|
| `app_id` | 配置的 APPID | 支付宝应用 ID |
| `method` | `alipay.trade.page.pay` 或 `alipay.trade.wap.pay` | 桌面网页 / 移动网页 |
| `format` | `JSON` | 固定 |
| `charset` | `utf-8` | 固定 |
| `sign_type` | `RSA2` | 固定 |
| `timestamp` | `YYYY-MM-DD HH:MM:SS` | 当前时间 |
| `version` | `1.0` | 固定 |
| `biz_content` | JSON 字符串 | 业务参数 |
| `notify_url` | 配置的异步通知 URL | 必须 HTTPS (生产) |
| `return_url` | 配置的同步跳转 URL | 必须 HTTPS (生产) |
| `sign` | RSA2 签名 | PKCS1v15 + SHA256 |

**biz_content 子字段：**

| 字段 | 值 |
|---|---|
| `out_trade_no` | `fs` + uuid4().hex (≈34 chars) |
| `product_code` | `FAST_INSTANT_TRADE_PAY` (桌面) / `QUICK_WAP_WAY` (移动) |
| `total_amount` | 挂牌价格 (格式化为 2 位小数) |
| `subject` | 挂牌标题 (截断到 128 字符) |
| `seller_id` | 配置的卖家 ID |

**前端提交方式（`platform.ts:350-363`）：**

```typescript
export function submitAlipayForm(form) {
  // 创建隐藏 HTML <form>，动态填入所有 fields
  // element.action = form.action  (支付宝网关 URL)
  // element.method = "POST"
  // 每个 field 作为 <input type="hidden">
  // document.body.append(element); element.submit()
  // → 浏览器全页跳转到支付宝收银台
}
```

### 2.3 异步通知回调处理

**通知入口：`POST /v1/webhooks/alipay`（`commerce/router.py:78-85`）**

```
支付宝服务器 → POST (application/x-www-form-urlencoded)
  → commerce/router.py: alipay_webhook()
    ├─ _alipay_form(request)
    │    ├─ 校验 Content-Type: application/x-www-form-urlencoded
    │    ├─ parse_qsl (ascii strict) → 提取 charset
    │    ├─ 支持 utf-8 / gbk / gb2312 编码
    │    ├─ parse_qsl (目标编码) → dict[str, str]
    │    └─ 重复键检测 → 拒绝
    └─ CommerceService.process_notification()

process_notification() 完整逻辑:
  ├─ _validate_notification_shape(fields)
  │    必需字段: app_id, seller_id, out_trade_no, total_amount,
  │              trade_no, trade_status, sign, sign_type
  │    任一为空 → 422
  ├─ 商户校验: fields["app_id"] == settings.alipay_app_id
  │            fields["seller_id"] == settings.alipay_seller_id
  │            不匹配 → 403
  ├─ AlipayPaymentGateway.verify_notification(fields)
  │    ├─ sign_type 必须是 "RSA2"
  │    ├─ base64 decode sign
  │    ├─ _canonical(fields): 按 key 排序 + "&" 连接 (排除 sign, sign_type, 空值)
  │    ├─ _provider_public_key().verify(signature, canonical_string, PKCS1v15, SHA256)
  │    └─ 失败 → AlipayProtocolError → 422
  ├─ _notification_trade(fields): Decimal 解析 total_amount + refund_amount
  ├─ fingerprint = SHA256(_notification_canonical(fields))
  │
  ├─ if trade_status == "TRADE_CLOSED":
  │    → _process_closed_notification()
  │       ├─ lock_attempt_by_out_trade_no (FOR UPDATE)
  │       ├─ insert_payment_notification (ON CONFLICT fingerprint DO NOTHING)
  │       ├─ if order pending_payment → close_unpaid
  │       └─ if order fulfilled → create_or_get_reversal → apply or manual_review
  │
  └─ else (TRADE_SUCCESS / TRADE_FINISHED):
       ├─ lock_attempt_by_out_trade_no (FOR UPDATE)  // 行级锁
       ├─ _validate_trade_against_attempt (out_trade_no + amount 一致性)
       ├─ insert_payment_notification (指纹去重)
       ├─ if TRADE_SUCCESS or TRADE_FINISHED:
       │    → _settle_locked()
       │       ├─ mark_settled: payment_attempts.status=succeeded
       │       │               orders.status=fulfilled, paid_at=now()
       │       ├─ grant_entitlements: INSERT entitlements (active)
       │       └─ record_sale: ledger_entries (creator_credit + platform_fee)
       │                      creator_balances.available_amount += creator_amount
       └─ return "success"  // 纯文本，告诉支付宝已收到
```

### 2.4 支付对账（Outbox Worker 异步）

**触发机制：**

```
1. checkout 创建时 → outbox_events:
   event_type="payment.reconcile.v1"
   available_at = now + payment_reconcile_delay_seconds (默认 60s)

2. CommerceService.schedule_due_work() 定时扫描:
   每 payment_reconcile_sweep_seconds (默认 300s)
   → 查找所有 status IN ('created','pending') 的 payment_attempts
   → 追加 payment.reconcile.v1 事件

3. OutboxWorker.poll_once() → 领取到期事件 → 调用 reconcile_event()
```

**对账状态机（`CommerceService.reconcile_pending_payment`）：**

```
query_trade(out_trade_no) → AlipayTrade
  │
  ├─ TRADE_SUCCESS / TRADE_FINISHED:
  │    → _settle_by_attempt() → fulfill order + grant entitlement + ledger
  │
  ├─ TRADE_CLOSED:
  │    ├─ order pending_payment → close_unpaid (order + attempt → closed)
  │    └─ order fulfilled → _reverse_paid() (冲正)
  │         ├─ 全额退款 → reverse_sale (ledger reversal)
  │         │            entitlements → revoked
  │         │            creator_balances.available_amount -= creator_amount
  │         └─ 部分退款 / 余额不足 → mark_reversal_manual_review
  │              order → payment_exception
  │
  ├─ WAIT_BUYER_PAY:
  │    ├─ 未过期 → RescheduleOutboxEvent(60s)
  │    └─ 已过期 → close_trade() → close_unpaid
  │
  └─ 其他状态 → RetryableOutboxError (指数退避重试，最多 10 次)
```

### 2.5 退款/冲正流程

**路径 A — 异步通知 TRADE_CLOSED（`service.py:290-342`）：**

```
支付宝发送 TRADE_CLOSED 通知
  → verify_notification (验签)
  → lock_attempt_by_out_trade_no (行锁)
  → insert_payment_notification (指纹去重)
  → if order fulfilled:
       create_or_get_reversal (ON CONFLICT payment_attempt_id)
       ├─ 全额 (refund_amount == order_amount):
       │    reverse_sale (ledger: creator_reversal + platform_reversal)
       │    entitlements → revoked
       │    order → payment_exception
       └─ 部分退款 / 余额不足:
            mark_reversal_manual_review → order → payment_exception
            (人工介入审查)
```

**路径 B — 主动查询对账（`service.py:189-202`）：**

```
Worker reconcile_event → query_trade → TRADE_CLOSED
  → 同路径 A 的冲正逻辑
  → 额外: 已过期 WAIT_BUYER_PAY → close_trade → close_unpaid
```

---

## 三、全部前后端交互 API

### 3.1 订单创建

| 项目 | 详情 |
|---|---|
| **前端调用** | `platform.commerce.checkout(listing)` |
| **HTTP** | `POST /v1/checkout` |
| **请求头** | `Content-Type: application/json`, `Idempotency-Key`, `X-CSRF-Token` |
| **请求体** | `{ "listingId": UUID, "licenceOfferId": UUID, "channel": "desktop_web" \| "mobile_web" }` |
| **成功响应** | `201` + `{ data: { order, paymentAttempt, alipayForm } }` |
| **幂等重放** | 相同 Idempotency-Key → 返回原响应 |
| **冲突错误** | `409 payment_already_pending` (同 listing 已有活跃订单) |
| **业务错误** | `404 published_offer_not_found`, `503 alipay_configuration_incomplete` |
| **前端后续** | `submitAlipayForm(response.alipayForm)` → 浏览器跳转支付宝 |

**响应 `alipayForm` 结构：**
```json
{
  "action": "https://openapi.alipay.com/gateway.do",
  "method": "POST",
  "fields": {
    "app_id": "...", "method": "alipay.trade.page.pay",
    "charset": "utf-8", "sign_type": "RSA2",
    "sign": "base64...", "biz_content": "{...}",
    "notify_url": "https://...", "return_url": "https://...",
    "timestamp": "2026-07-28 12:00:00", "version": "1.0",
    "format": "JSON"
  }
}
```

### 3.2 订单查询

| 项目 | 详情 |
|---|---|
| **前端调用** | `platform.commerce.order(orderId)` |
| **HTTP** | `GET /v1/orders/{orderId}` |
| **鉴权** | 仅订单买家可查 (buyer_id 隔离) |
| **响应** | `{ data: OrderView }` |
| **OrderView 字段** | id, status, amount, currency, paidAt, createdAt, items[] |

### 3.3 购买列表

| 项目 | 详情 |
|---|---|
| **前端调用** | `platform.commerce.purchases()` |
| **HTTP** | `GET /v1/me/purchases?limit=48` |
| **分页** | cursor 分页 (base64 编码 `{v:1, kind:"purchases", after:{createdAt, id}}`) |
| **响应** | `{ data: OrderView[], page: { nextCursor } }` |

### 3.4 下载已购资产

| 项目 | 详情 |
|---|---|
| **前端调用** | `platform.assets.downloadUrl(assetId)` |
| **HTTP** | `POST /v1/assets/{assetId}/download-url` |
| **鉴权** | 需要 effective entitlement (entitlements.status=active) |
| **响应** | `{ data: { url: "预签名 S3 URL", expiresAt: "ISO datetime" } }` |
| **TTL** | 5 分钟 (`master_download_ttl_seconds=300`) |
| **401** | 未购买/entitlement 已撤销 → 拒绝 |

### 3.5 异步通知 Webhook

| 项目 | 详情 |
|---|---|
| **发起方** | 支付宝服务器 |
| **HTTP** | `POST /v1/webhooks/alipay` |
| **Content-Type** | `application/x-www-form-urlencoded` |
| **鉴权** | RSA2 签名验证 (无 session/csrf) |
| **响应** | `200 "success"` (纯文本) |
| **幂等** | fingerprint = SHA256(canonical fields) → payment_notifications 唯一约束 |
| **错误** | 422 (验签失败/格式错误), 403 (商户不匹配), 404 (订单不存在), 409 (金额不匹配) |
| **注意** | 支付宝要求返回 "success" 字符串，否则会重发通知 |

### 3.6 支付状态流转总图

```
┌─────────────┐     ┌──────────────────┐     ┌─────────────────────┐
│ checkout    │────▶│ order:           │     │ payment_attempt:    │
│ POST /v1/   │     │ pending_payment  │────▶│ created             │
│ checkout    │     └────────┬─────────┘     └──────────┬──────────┘
└─────────────┘              │                           │
                             │ 支付宝异步通知              │ Worker 对账查询
                             │ TRADE_SUCCESS             │ query_trade()
                             ▼                           ▼
                    ┌─────────────────┐        ┌──────────────────┐
                    │ order: fulfilled│        │ attempt:         │
                    │ paid_at = now() │        │ succeeded        │
                    └────────┬────────┘        └──────────────────┘
                             │
                             │ TRADE_CLOSED (退款)
                             ▼
                    ┌─────────────────────┐
                    │ order:              │
                    │ payment_exception   │
                    │ (全额退款→冲正)      │
                    │ (部分退款→人工审查)   │
                    └─────────────────────┘

超时未支付:
  WAIT_BUYER_PAY + expires_at < now()
    → close_trade() → close_unpaid → order: closed

付款前取消:
  TRADE_CLOSED + order pending_payment
    → close_unpaid → order: closed
```

---

## 四、安全问题审计

### 4.1 ✅ 已做好的安全措施

| # | 措施 | 实现位置 |
|---|---|---|
| 1 | **RSA2 签名验证** — 异步通知强制 RSA2+SHA256 验签，reject 非 RSA2 | `payment_gateway.py:76-88` |
| 2 | **网关响应二次验签** — `_verified_gateway_response` 对支付宝同步响应 JSON body 做签名验证 | `payment_gateway.py:137-151` |
| 3 | **商户身份校验** — 通知 app_id + seller_id 必须在结算前匹配 | `service.py:147-148` |
| 4 | **交易金额校验** — `_validate_trade_against_attempt` 校验 out_trade_no + total_amount | `service.py:365-367` |
| 5 | **指纹去重** — 通知 SHA256 指纹→`payment_notifications` 唯一约束，防止重复处理 | `service.py:151` |
| 6 | **金额范围约束** — price ∈ [0.01, 1,000,000.00]，Decimal 精度校验到分 | `service.py:61-64` |
| 7 | **DB 行级锁** — `lock_attempt_by_out_trade_no` / `lock_attempt_by_id` 使用 `FOR UPDATE` | `repository.py:218-228` |
| 8 | **幂等下单** — `Idempotency-Key` + DB 租赁，防重复扣款 | `service.py:73-78` |
| 9 | **CSRF 保护** — 所有写操作要求 `X-CSRF-Token` (HMAC-SHA256) | `commerce/router.py:67` |
| 10 | **生产环境强制校验** — prod 模式：HTTPS origin、非 stub 支付宝、S3 加密 | `config.py:88-110` |
| 11 | **私钥不落日志** — 通知 payload 只记录 redacted 版本 (字段名+哈希) | `service.py:374-377` |
| 12 | **订单快照不可变** — `order_items` 存储 listing+licence JSONB 快照，历史订单不受后续价格变更影响 | `repository.py:121-126` |
| 13 | **账本不可变** — `ledger_entries` append-only + 唯一约束防重复过账 | `sale_ledger_writer.py:28-31` |
| 14 | **余额非负约束** — `creator_balances` CHECK available_amount >= 0, reserved_amount >= 0 | migration |
| 15 | **端到端测试覆盖** — 签名伪造拒绝、金额篡改拒绝、商户伪造拒绝、部分退款人工审查 | `test_alipay_settlement.py` |

### 4.2 ⚠️ 现存安全风险

| # | 风险 | 严重度 | 位置 | 详情 |
|---|---|---|---|---|
| R1 | **Stub 模式签名可预测** | 中 | `payment_gateway.py:132` | `"dev-stub-" + SHA256(canonical)` 是确定性的，攻击者可计算有效签名。仅开发环境启用且生产有 `ALIPAY_STUB_MODE=false` 强制校验，风险可控。 |
| R2 | **私钥文件无密码保护** | 低 | `payment_gateway.py:188` | `load_pem_private_key(key_path.read_bytes(), password=None)` — 私钥 PEM 文件无密码加密。依赖文件系统权限。 |
| R3 | **支付宝公钥无缓存失效机制** | 低 | `payment_gateway.py:49` | `self._public_key` 缓存后永不过期。若支付宝更换公钥需重启服务。生产环境公钥更换是罕见事件。 |
| R4 | **Stub 公钥通过 HTTP 获取** | 低 | `payment_gateway.py:161-163` | Stub 模式下公钥通过 `httpx.get(alipay_stub_public_key_url)` 获取，URL 通常指向同 Compose 网络的 stub 容器。开发环境可控。 |
| R5 | **Webhook 端点在 OpenAPI schema 中隐藏** | 信息 | `commerce/router.py:78` | `include_in_schema=False` 意味着 `/v1/webhooks/alipay` 不在自动生成的 API 文档中。这是正确的安全做法。 |
| R6 | **OrderItem 无 buyer_id 直接字段** | 信息 | migration | `order_items` 不含 `buyer_id`，通过 `orders.buyer_id` JOIN 获取。授权检查通过 `entitlements` 表间接实现。当前实现正确但间接。 |

### 4.3 沙箱/生产环境隔离评估

| 维度 | 当前实现 | 评估 |
|---|---|---|
| **Stub 模式开关** | `ALIPAY_STUB_MODE` 环境变量 | ✅ 明确的环境开关 |
| **生产强制校验** | `config.py:103-104`: prod 下 `alipay_stub_mode=true` → 启动报错 | ✅ 安全 |
| **密钥路径分离** | 生产读磁盘 PEM，Stub 读 HTTP URL | ✅ 隔离正确 |
| **网关 URL 分离** | 生产 `openapi.alipay.com`，Stub `localhost:18102` | ✅ 隔离正确 |
| **签名算法** | 生产 RSA2-PKCS1v15，Stub SHA256+前缀 | ✅ 隔离正确 |
| **通知 URL** | 生产 HTTPS 强制 (prod 校验含 `notify_url.startswith("https://")`) | ✅ 安全 |
| **Stub 私钥生成** | 每次启动生成新的 2048-bit 密钥对 (`alipay_stub.py:17`) | ✅ 确定性低风险 |
| **通用设置复用** | `ALIPAY_TIMEOUT_SECONDS`, `ALIPAY_MAX_TOTAL_AMOUNT` 等共用 | ⚠️ 共用合理但需注意 |

---

## 五、前端页面开发思路与完整代码

### 5.1 现有前端支付页面分析

**已有页面：**

| 页面 | 文件 | 功能完整度 |
|---|---|---|
| 探索/购买 | `explore/page.tsx` | ✅ 浏览+收藏+支付宝支付按钮 |
| 已购订单 | `purchases/page.tsx` | ✅ 订单列表+已购资产下载 |

**缺失的支付相关页面：**
- ❌ 支付结果页（同步跳转 return_url 后的展示）
- ❌ 订单详情页（独立路由，非 explore page 内联）

### 5.2 需要开发的前端页面

根据后端已完备的接口，建议开发以下页面：

#### 页面 1：支付结果页 `/(workbench)/payment-result/page.tsx`

支付宝支付完成后浏览器通过 `return_url` 跳回。当前项目无此页面。

#### 页面 2：增强型订单详情组件

在 explore page 中 checkout 后展示订单状态。

### 5.3 完整前端页面代码

#### 支付结果页

```typescript
// frontend/src/app/[locale]/(workbench)/payment-result/page.tsx
"use client";

import { useEffect, useState, useCallback } from "react";
import { useSearchParams } from "next/navigation";
import { useTranslations } from "next-intl";
import { CheckCircle, XCircle, Clock, Loader2, ExternalLink } from "lucide-react";
import { Link } from "@/i18n/navigation";
import { Button } from "@/components/ui/button";
import { Card } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { Skeleton } from "@/components/ui/skeleton";
import { toast } from "@/components/ui/toaster";
import { platform, type Order } from "@/lib/api/platform";

type PaymentStatus =
  | "verifying"       // 正在查询支付宝确认结果
  | "success"         // 支付成功
  | "pending"         // 支付处理中（银行延迟）
  | "closed"          // 交易关闭/超时
  | "exception"       // 异常
  | "not_found";      // 订单未找到

function statusConfig(status: PaymentStatus): {
  icon: React.ReactNode;
  titleKey: string;
  descriptionKey: string;
  badgeVariant: "success" | "destructive" | "warning" | "running";
  badgeKey: string;
} {
  switch (status) {
    case "verifying":
      return {
        icon: <Loader2 className="h-12 w-12 animate-spin text-neon-cyan" />,
        titleKey: "result.verifying.title",
        descriptionKey: "result.verifying.description",
        badgeVariant: "running",
        badgeKey: "result.verifying.badge",
      };
    case "success":
      return {
        icon: <CheckCircle className="h-12 w-12 text-green-400" />,
        titleKey: "result.success.title",
        descriptionKey: "result.success.description",
        badgeVariant: "success",
        badgeKey: "result.success.badge",
      };
    case "pending":
      return {
        icon: <Clock className="h-12 w-12 text-amber-400" />,
        titleKey: "result.pending.title",
        descriptionKey: "result.pending.description",
        badgeVariant: "warning",
        badgeKey: "result.pending.badge",
      };
    case "closed":
      return {
        icon: <XCircle className="h-12 w-12 text-gray-400" />,
        titleKey: "result.closed.title",
        descriptionKey: "result.closed.description",
        badgeVariant: "destructive",
        badgeKey: "result.closed.badge",
      };
    case "exception":
      return {
        icon: <XCircle className="h-12 w-12 text-red-400" />,
        titleKey: "result.exception.title",
        descriptionKey: "result.exception.description",
        badgeVariant: "destructive",
        badgeKey: "result.exception.badge",
      };
    case "not_found":
      return {
        icon: <XCircle className="h-12 w-12 text-gray-400" />,
        titleKey: "result.notFound.title",
        descriptionKey: "result.notFound.description",
        badgeVariant: "destructive",
        badgeKey: "result.notFound.badge",
      };
  }
}

function OrderItemCard({ order }: { order: Order }) {
  const t = useTranslations("commerce");
  const [downloading, setDownloading] = useState(false);

  const download = useCallback(async (assetId: string) => {
    setDownloading(true);
    try {
      const { url } = await platform.assets.downloadUrl(assetId);
      window.open(url, "_blank", "noopener,noreferrer");
      toast({ title: t("result.downloadStarted"), variant: "success" });
    } catch {
      toast({ title: t("errors.downloadFailed"), variant: "destructive" });
    } finally {
      setDownloading(false);
    }
  }, [t]);

  if (order.status !== "fulfilled") return null;

  return (
    <Card className="p-5 space-y-3">
      <h3 className="font-medium">{t("result.downloadAssets")}</h3>
      {order.items.map((item) => (
        <div key={item.assetId} className="flex items-center justify-between">
          <div>
            <p className="text-sm text-muted-foreground">{item.assetId}</p>
            <p className="text-sm">{item.price} CNY</p>
          </div>
          <Button
            size="sm"
            variant="fractal"
            loading={downloading}
            onClick={() => void download(item.assetId)}
          >
            <ExternalLink className="mr-1.5 h-3.5 w-3.5" />
            {t("actions.downloadAsset")}
          </Button>
        </div>
      ))}
    </Card>
  );
}

export default function PaymentResultPage() {
  const t = useTranslations("commerce");
  const searchParams = useSearchParams();
  const [status, setStatus] = useState<PaymentStatus>("verifying");
  const [order, setOrder] = useState<Order | null>(null);

  useEffect(() => {
    let cancelled = false;
    // 支付宝 return_url 通常会带上 out_trade_no 参数
    const outTradeNo = searchParams.get("out_trade_no");

    const verify = async () => {
      if (!outTradeNo) {
        if (!cancelled) setStatus("not_found");
        return;
      }
      // 轮询最近购买直到状态确定（最多 30 秒）
      for (let attempt = 0; attempt < 10; attempt++) {
        if (cancelled) return;
        try {
          const page = await platform.commerce.purchases();
          const found = page.data.find((o) =>
            o.id === outTradeNo || // 如果 return_url 传的是 order_id
            true // 否则取最新的
          );
          if (found) {
            if (!cancelled) {
              setOrder(found);
              switch (found.status) {
                case "fulfilled": setStatus("success"); return;
                case "pending_payment": break; // 继续等待
                case "closed": setStatus("closed"); return;
                case "payment_exception": setStatus("exception"); return;
              }
            }
          }
        } catch {
          // 网络错误，继续重试
        }
        await new Promise((r) => setTimeout(r, 3000));
      }
      if (!cancelled) setStatus("pending");
    };

    void verify();
    return () => { cancelled = true; };
  }, [searchParams]);

  const cfg = statusConfig(status);

  return (
    <div className="flex min-h-[60vh] items-center justify-center">
      <div className="w-full max-w-md space-y-6 text-center">
        <div className="flex justify-center">{cfg.icon}</div>
        <div>
          <h1 className="text-xl font-semibold">{t(cfg.titleKey)}</h1>
          <p className="mt-2 text-sm text-muted-foreground">
            {t(cfg.descriptionKey)}
          </p>
        </div>

        {status === "verifying" && (
          <div className="space-y-3">
            <Skeleton className="mx-auto h-16 w-64" />
            <Skeleton className="mx-auto h-4 w-48" />
          </div>
        )}

        {order && (
          <div className="text-left">
            <div className="mb-2 flex items-center justify-between">
              <span className="text-sm text-muted-foreground">{t("order.id")}</span>
              <Badge variant={cfg.badgeVariant}>{t(cfg.badgeKey)}</Badge>
            </div>
            <p className="break-all text-xs text-muted-foreground">{order.id}</p>
            <p className="mt-1 text-lg font-bold">
              {order.amount} {order.currency}
            </p>
            {order.paidAt && (
              <p className="text-xs text-muted-foreground">
                {t("order.paidAt")}: {new Date(order.paidAt).toLocaleString()}
              </p>
            )}
          </div>
        )}

        {order?.status === "fulfilled" && <OrderItemCard order={order} />}

        <div className="flex justify-center gap-3">
          <Link href="/purchases">
            <Button variant="outline">{t("result.viewPurchases")}</Button>
          </Link>
          <Link href="/explore">
            <Button variant="fractal">{t("result.backToExplore")}</Button>
          </Link>
        </div>
      </div>
    </div>
  );
}
```

#### 增强型订单详情卡片组件

```typescript
// frontend/src/components/commerce/order-status-card.tsx
"use client";

import { useState, useCallback } from "react";
import { useTranslations } from "next-intl";
import {
  Clock, CheckCircle, XCircle, AlertTriangle,
  ExternalLink, RefreshCw, ShoppingCart,
} from "lucide-react";
import { Button } from "@/components/ui/button";
import { Card } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { Skeleton } from "@/components/ui/skeleton";
import { toast } from "@/components/ui/toaster";
import { platform, type Order, type Listing } from "@/lib/api/platform";
import { formatDate } from "@/lib/utils/format";

type OrderStatus = Order["status"];

const ORDER_STATUS_MAP: Record<
  OrderStatus,
  { icon: React.ReactNode; labelKey: string; variant: "warning" | "success" | "destructive" | "running" | "info" }
> = {
  pending_payment: {
    icon: <Clock className="h-4 w-4" />,
    labelKey: "orderStatus.pending_payment",
    variant: "warning",
  },
  fulfilled: {
    icon: <CheckCircle className="h-4 w-4" />,
    labelKey: "orderStatus.fulfilled",
    variant: "success",
  },
  closed: {
    icon: <XCircle className="h-4 w-4" />,
    labelKey: "orderStatus.closed",
    variant: "destructive",
  },
  payment_exception: {
    icon: <AlertTriangle className="h-4 w-4" />,
    labelKey: "orderStatus.payment_exception",
    variant: "destructive",
  },
};

interface OrderStatusCardProps {
  orderId: string;
  listing?: Listing | null;
  onRefresh?: (order: Order) => void;
}

export function OrderStatusCard({ orderId, listing, onRefresh }: OrderStatusCardProps) {
  const t = useTranslations("commerce");
  const [order, setOrder] = useState<Order | null>(null);
  const [isLoading, setIsLoading] = useState(true);
  const [isRefreshing, setIsRefreshing] = useState(false);
  const [downloading, setDownloading] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const fetchOrder = useCallback(async () => {
    setError(null);
    try {
      const data = await platform.commerce.order(orderId);
      setOrder(data);
      onRefresh?.(data);
    } catch {
      setError(t("errors.orderNotFound"));
    } finally {
      setIsLoading(false);
    }
  }, [orderId, t, onRefresh]);

  const refresh = useCallback(async () => {
    setIsRefreshing(true);
    await fetchOrder();
    setIsRefreshing(false);
  }, [fetchOrder]);

  // 初次加载
  useState(() => { void fetchOrder(); });

  const download = useCallback(async (assetId: string) => {
    setDownloading(assetId);
    try {
      const { url } = await platform.assets.downloadUrl(assetId);
      window.open(url, "_blank", "noopener,noreferrer");
      toast({ title: t("result.downloadStarted"), variant: "success" });
    } catch {
      toast({ title: t("errors.downloadFailed"), variant: "destructive" });
    } finally {
      setDownloading(null);
    }
  }, [t]);

  if (isLoading) {
    return (
      <Card className="p-5 space-y-3">
        <Skeleton className="h-5 w-32" />
        <Skeleton className="h-4 w-48" />
        <Skeleton className="h-10 w-full" />
      </Card>
    );
  }

  if (error || !order) {
    return (
      <Card className="p-5 text-center">
        <p className="text-sm text-muted-foreground">{error ?? t("errors.orderNotFound")}</p>
      </Card>
    );
  }

  const statusCfg = ORDER_STATUS_MAP[order.status];

  return (
    <Card className="p-5 space-y-4">
      {/* 头部 */}
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-2">
          <ShoppingCart className="h-4 w-4 text-muted-foreground" />
          <h3 className="font-medium">{t("order.title")}</h3>
        </div>
        <Button size="icon" variant="ghost" loading={isRefreshing} onClick={() => void refresh()}>
          <RefreshCw className="h-4 w-4" />
        </Button>
      </div>

      {/* 状态 */}
      <div className="flex items-center gap-2">
        {statusCfg.icon}
        <Badge variant={statusCfg.variant}>{t(statusCfg.labelKey)}</Badge>
      </div>

      {/* 金额 */}
      <div className="flex items-baseline gap-1">
        <span className="text-2xl font-bold">{order.amount}</span>
        <span className="text-sm text-muted-foreground">{order.currency}</span>
      </div>

      {/* 挂牌信息 */}
      {listing && (
        <div className="rounded-lg bg-white/5 p-3 text-sm space-y-1">
          <p className="font-medium truncate">{listing.title}</p>
          <p className="text-muted-foreground">
            {t("marketplace.byCreator", { creator: listing.creator.displayName })}
          </p>
        </div>
      )}

      {/* 时间 */}
      <div className="text-xs text-muted-foreground space-y-0.5">
        <p>{t("order.createdAt")}: {formatDate(order.createdAt)}</p>
        {order.paidAt && <p>{t("order.paidAt")}: {formatDate(order.paidAt)}</p>}
      </div>

      {/* 操作区 */}
      {order.status === "fulfilled" && order.items.map((item) => (
        <Button
          key={item.assetId}
          className="w-full"
          variant="fractal"
          loading={downloading === item.assetId}
          onClick={() => void download(item.assetId)}
        >
          <ExternalLink className="mr-1.5 h-4 w-4" />
          {t("actions.downloadAsset")} ({item.price} CNY)
        </Button>
      ))}

      {order.status === "pending_payment" && (
        <p className="text-sm text-amber-400">{t("order.awaitingPayment")}</p>
      )}

      {order.status === "payment_exception" && (
        <p className="text-sm text-red-400">{t("order.exceptionHint")}</p>
      )}
    </Card>
  );
}
```

#### 增强型 Explore Page（含订单状态跟踪）

```typescript
// 在 explore/page.tsx 中 checkout() 函数增强
// 原有代码 + 以下增强:

const [activeCheckout, setActiveCheckout] = useState<{
  orderId: string;
  listingTitle: string;
} | null>(null);

const checkout = async (listing: Listing) => {
  try {
    const result = await platform.commerce.checkout(listing);
    setActiveCheckout({
      orderId: result.order.id,
      listingTitle: listing.title,
    });
    // 支付跳转
    submitAlipayForm(result.alipayForm);
  } catch (reason: unknown) {
    const err = reason as { code?: string };
    if (err?.code === "payment_already_pending") {
      toast({
        title: t("errors.alreadyPending"),
        description: t("errors.alreadyPendingDescription"),
        variant: "warning",
      });
    } else {
      setError(t("errors.requestFailed"));
    }
  }
};

// 如支付宝 return_url 未配置或不跳回，可在页面中轮询展示订单状态
useEffect(() => {
  if (!activeCheckout) return;
  const interval = setInterval(async () => {
    try {
      const order = await platform.commerce.order(activeCheckout.orderId);
      if (order.status === "fulfilled") {
        toast({
          title: t("result.paymentSuccess"),
          description: t("result.paymentSuccessDescription", { title: activeCheckout.listingTitle }),
          variant: "success",
        });
        setActiveCheckout(null);
      } else if (order.status === "closed" || order.status === "payment_exception") {
        setActiveCheckout(null);
      }
    } catch { /* 忽略轮询错误 */ }
  }, 5000);
  return () => clearInterval(interval);
}, [activeCheckout, t]);
```

### 5.4 国际化文案补充

```json
// frontend/messages/zh.json — commerce 命名空间新增
{
  "commerce": {
    "result": {
      "verifying": { "title": "正在确认支付结果", "description": "正在查询支付宝支付状态，请稍候…", "badge": "验证中" },
      "success": { "title": "支付成功", "description": "资产已添加至您的库中，可以随时下载。", "badge": "已支付" },
      "pending": { "title": "支付处理中", "description": "银行处理可能需要几分钟，请稍后查看。", "badge": "处理中" },
      "closed": { "title": "交易已关闭", "description": "支付未在规定时间内完成或已被取消。", "badge": "已关闭" },
      "exception": { "title": "支付异常", "description": "支付过程中出现异常，请联系客服。", "badge": "异常" },
      "notFound": { "title": "订单未找到", "description": "未找到对应订单信息。", "badge": "未找到" },
      "downloadStarted": "开始下载",
      "downloadAssets": "下载已购资产",
      "viewPurchases": "查看已购",
      "backToExplore": "返回探索",
      "paymentSuccess": "支付成功！",
      "paymentSuccessDescription": "{title} 已添加至您的资产库。"
    },
    "order": {
      "title": "订单详情",
      "id": "订单号",
      "createdAt": "创建时间",
      "paidAt": "支付时间",
      "awaitingPayment": "等待支付宝支付确认…",
      "exceptionHint": "请联系客服处理此异常订单。"
    },
    "orderStatus": {
      "pending_payment": "待支付",
      "fulfilled": "已完成",
      "closed": "已关闭",
      "payment_exception": "异常"
    },
    "errors": {
      "orderNotFound": "订单未找到",
      "alreadyPending": "已有待处理订单",
      "alreadyPendingDescription": "该资产已有活跃支付，请完成或等待其过期。",
      "downloadFailed": "下载失败，请重试。"
    }
  }
}
```

---

## 六、支付系统总结

### 6.1 架构优势

1. **完整的状态机**：Order (4 状态) × PaymentAttempt (5 状态) 覆盖正常支付、超时关闭、退款冲正全路径
2. **双重保障**：支付宝异步通知 + Worker 主动查询对账，防止通知丢失
3. **不可变审计**：订单快照 (JSONB) + 账本 append-only + 审计事件 triple-entry
4. **安全纵深**：RSA2 验签 → 商户校验 → 金额校验 → 指纹去重 → 行锁 → 幂等
5. **开发友好**：完整 RSA2 Stub 可独立运行，支持通知模拟、交易状态注入、签名验证
6. **E2E 覆盖充分**：test_alipay_settlement.py 覆盖成功/关闭/部分退款/余额不足/伪造签名/错误商户等场景

### 6.2 待改进项

1. **支付宝 return_url 跳回页面缺失** — 需实现 `payment-result/page.tsx`
2. **订单状态轮询** — explore page 中 checkout 后缺少自动轮询机制
3. **私钥密码保护** — 生产环境建议使用密码保护的 PEM 文件
4. **对账监控告警** — `payment_exception` 和 `manual_review` 状态缺少通知机制（邮件/Webhook）

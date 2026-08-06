"""Local RSA2-capable Alipay contract stub. Development and E2E only.

Browser sandbox flow:
  1. Browser POSTs form to /gateway.do  →  returns an HTML payment simulation page
  2. User clicks "Confirm Payment"  →  stub auto-sends signed notification to Platform webhook
  3. Stub redirects browser to return_url with out_trade_no
"""

from __future__ import annotations

import base64
import json
from decimal import Decimal
from typing import Any

import httpx
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from fastapi import FastAPI, Form, HTTPException
from fastapi.responses import HTMLResponse, PlainTextResponse


app = FastAPI()
_private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
_trades: dict[str, dict[str, str]] = {}


def _canonical(fields: dict[str, str]) -> str:
    return "&".join(f"{key}={fields[key]}" for key in sorted(fields) if key not in {"sign", "sign_type"} and fields[key] != "")


def _sign(content: str) -> str:
    return base64.b64encode(_private_key.sign(content.encode(), padding.PKCS1v15(), hashes.SHA256())).decode()


def _trade(out_trade_no: str) -> dict[str, str]:
    return _trades.setdefault(out_trade_no, {
        "out_trade_no": out_trade_no, "trade_no": f"stub-{out_trade_no[-18:]}",
        "trade_status": "WAIT_BUYER_PAY", "total_amount": "1.00",
    })


async def _send_notification_to_platform(
    out_trade_no: str, trade_status: str, notify_url: str, refund_amount: str | None = None
) -> bool:
    """Send a signed async notification to the Platform webhook, mimicking real Alipay."""
    if not notify_url:
        return False
    trade = _trade(out_trade_no)
    fields: dict[str, str] = {
        "app_id": "dev-stub", "charset": "utf-8", "seller_id": "dev-stub",
        "out_trade_no": out_trade_no, "total_amount": trade["total_amount"],
        "trade_no": trade["trade_no"], "trade_status": trade_status,
        "sign_type": "RSA2", "notify_time": "2026-07-24 00:00:00",
    }
    if refund_amount:
        fields["refund_amount"] = refund_amount
    fields["sign"] = _sign(_canonical(fields))
    try:
        async with httpx.AsyncClient(timeout=10, trust_env=False) as client:
            response = await client.post(notify_url, data=fields)
            return response.status_code == 200 and response.text.strip() == "success"
    except httpx.HTTPError:
        return False


_PAYMENT_PAGE_HTML = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>支付宝沙箱 — 模拟收银台</title>
<style>
  *,*::before,*::after{{box-sizing:border-box;margin:0;padding:0}}
  body{{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
        background:linear-gradient(135deg,#0f172a 0%,#1e293b 100%);color:#e2e8f0;
        min-height:100vh;display:flex;align-items:center;justify-content:center}}
  .card{{background:rgba(255,255,255,0.05);border:1px solid rgba(255,255,255,0.1);
          border-radius:16px;padding:32px;width:100%;max-width:420px;backdrop-filter:blur(12px)}}
  .card h2{{font-size:20px;margin-bottom:4px}}
  .card .subtitle{{color:#94a3b8;font-size:13px;margin-bottom:24px}}
  .row{{display:flex;justify-content:space-between;padding:10px 0;border-bottom:1px solid rgba(255,255,255,0.06);font-size:14px}}
  .row .label{{color:#94a3b8}}
  .row .value{{font-weight:600}}
  .amount{{font-size:28px;font-weight:700;color:#36f0e8;text-align:center;margin:20px 0}}
  .badge{{display:inline-block;padding:2px 10px;border-radius:99px;font-size:12px;background:rgba(147,51,255,0.2);color:#c4a8ff}}
  button{{width:100%;padding:14px;border:none;border-radius:10px;font-size:16px;font-weight:600;cursor:pointer;margin-top:20px;transition:all .2s}}
  .btn-pay{{background:linear-gradient(135deg,#7c22e0,#9333ff);color:#fff}}
  .btn-pay:hover{{transform:translateY(-1px);box-shadow:0 8px 24px rgba(147,51,255,.3)}}
  .btn-pay:disabled{{opacity:.5;cursor:not-allowed;transform:none}}
  .btn-cancel{{background:rgba(255,255,255,.06);color:#94a3b8;margin-top:8px}}
  .btn-cancel:hover{{background:rgba(255,255,255,.1)}}
  .result{{text-align:center;padding:20px 0;display:none}}
  .result.show{{display:block}}
  .result .icon{{font-size:48px;margin-bottom:12px}}
  .spinner{{display:inline-block;width:20px;height:20px;border:2px solid rgba(255,255,255,.3);
            border-top-color:#fff;border-radius:50%;animation:spin .6s linear infinite;vertical-align:middle;margin-right:8px}}
  @keyframes spin{{to{{transform:rotate(360deg)}}}}
</style>
</head>
<body>
<div class="card">
  <h2>🪙 支付宝沙箱模拟</h2>
  <p class="subtitle">本地开发环境 — 不会产生真实交易</p>
  <div class="row"><span class="label">订单号</span><span class="value" id="tradeNo">—</span></div>
  <div class="row"><span class="label">商品</span><span class="value" id="subject">—</span></div>
  <div class="amount" id="amount">—</div>
  <div style="text-align:center"><span class="badge">沙箱环境</span></div>
  <div id="actions">
    <button class="btn-pay" id="payBtn" onclick="confirmPayment()">确认支付</button>
    <button class="btn-cancel" onclick="cancelPayment()">取消</button>
  </div>
  <div id="result" class="result">
    <div class="icon" id="resultIcon"></div>
    <p id="resultText"></p>
  </div>
</div>
<script>
  const OUT_TRADE_NO = "{out_trade_no}";
  const NOTIFY_URL = "{notify_url}";
  const RETURN_URL = "{return_url}";
  const TOTAL_AMOUNT = "{total_amount}";
  const SUBJECT = "{subject}";
  document.getElementById("tradeNo").textContent = OUT_TRADE_NO.slice(-16);
  document.getElementById("subject").textContent = SUBJECT;
  document.getElementById("amount").textContent = "¥ " + TOTAL_AMOUNT;

  async function confirmPayment() {{
    const btn = document.getElementById("payBtn");
    btn.disabled = true;
    btn.innerHTML = '<span class="spinner"></span>处理中…';
    try {{
      const resp = await fetch("/gateway.do/pay", {{
        method: "POST",
        headers: {{"Content-Type": "application/json"}},
        body: JSON.stringify({{ outTradeNo: OUT_TRADE_NO }})
      }});
      const data = await resp.json();
      if (data.success) {{
        document.getElementById("actions").style.display = "none";
        document.getElementById("result").classList.add("show");
        document.getElementById("resultIcon").textContent = "✅";
        document.getElementById("resultText").innerHTML =
          "支付成功！<br><small style='color:#94a3b8'>通知已发送 · 即将跳转…</small>";
        if (RETURN_URL) {{
          setTimeout(function() {{
            window.location.href = RETURN_URL +
              (RETURN_URL.includes("?") ? "&" : "?") +
              "out_trade_no=" + encodeURIComponent(OUT_TRADE_NO);
          }}, 1500);
        }}
      }} else {{
        document.getElementById("result").classList.add("show");
        document.getElementById("resultIcon").textContent = "❌";
        document.getElementById("resultText").textContent =
          "通知发送失败: " + (data.error || "未知错误");
        btn.disabled = false;
        btn.textContent = "重试";
      }}
    }} catch(e) {{
      btn.disabled = false;
      btn.textContent = "重试";
    }}
  }}

  function cancelPayment() {{
    if (RETURN_URL) {{
      window.location.href = RETURN_URL +
        (RETURN_URL.includes("?") ? "&" : "?") +
        "out_trade_no=" + encodeURIComponent(OUT_TRADE_NO) +
        "&status=cancelled";
    }} else {{
      document.getElementById("actions").style.display = "none";
      document.getElementById("result").classList.add("show");
      document.getElementById("resultIcon").textContent = "🚫";
      document.getElementById("resultText").textContent = "已取消支付";
    }}
  }}
</script>
</body>
</html>"""


@app.get("/test/public-key", response_class=PlainTextResponse)
async def public_key() -> str:
    return _private_key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo
    ).decode()


@app.put("/test/trades/{out_trade_no}")
async def set_trade(out_trade_no: str, body: dict[str, Any]) -> dict[str, str]:
    trade = _trade(out_trade_no)
    status = str(body.get("tradeStatus", trade["trade_status"]))
    amount = Decimal(str(body.get("totalAmount", trade["total_amount"]))).quantize(Decimal("0.01"))
    if status not in {"WAIT_BUYER_PAY", "TRADE_SUCCESS", "TRADE_FINISHED", "TRADE_CLOSED"} or amount <= 0:
        raise HTTPException(status_code=422)
    trade.update({"trade_status": status, "total_amount": format(amount, ".2f")})
    if body.get("tradeNo"):
        trade["trade_no"] = str(body["tradeNo"])
    if body.get("refundAmount") is not None:
        trade["refund_amount"] = format(Decimal(str(body["refundAmount"])).quantize(Decimal("0.01")), ".2f")
    else:
        trade.pop("refund_amount", None)
    return trade


@app.post("/test/notifications")
async def notification(body: dict[str, Any]) -> dict[str, str]:
    out_trade_no = str(body["outTradeNo"])
    trade = _trade(out_trade_no)
    if body.get("tradeStatus") is not None:
        await set_trade(out_trade_no, body)
    fields = {
        "app_id": str(body.get("appId", "dev-stub")), "charset": "utf-8", "seller_id": str(body.get("sellerId", "dev-stub")),
        "out_trade_no": out_trade_no, "total_amount": trade["total_amount"], "trade_no": trade["trade_no"],
        "trade_status": trade["trade_status"], "sign_type": "RSA2", "notify_time": "2026-07-24 00:00:00",
    }
    if "refund_amount" in trade:
        fields["refund_amount"] = trade["refund_amount"]
    fields["sign"] = _sign(_canonical(fields))
    return fields


@app.post("/gateway.do", response_model=None)
async def gateway(
    method: str = Form(...),
    biz_content: str = Form(...),
    notify_url: str = Form(default="", alias="notify_url"),
    return_url: str = Form(default="", alias="return_url"),
) -> HTMLResponse | dict[str, object]:
    """Serve both browser checkout and Alipay's JSON API on its shared endpoint."""
    if method in {"alipay.trade.query", "alipay.trade.close"}:
        return _gateway_json_response(method, biz_content)
    payload = json.loads(biz_content)
    out_trade_no = str(payload["out_trade_no"])
    trade = _trade(out_trade_no)
    # Persist notify/return URLs so /gateway.do/pay can use them
    trade["_notify_url"] = notify_url
    trade["_return_url"] = return_url
    trade["_subject"] = str(payload.get("subject", "Fractal Studio asset"))
    trade["total_amount"] = str(payload.get("total_amount", trade["total_amount"]))
    html = _PAYMENT_PAGE_HTML.format(
        out_trade_no=out_trade_no,
        notify_url=notify_url,
        return_url=return_url,
        total_amount=trade["total_amount"],
        subject=trade["_subject"],
    )
    return HTMLResponse(content=html)


@app.post("/gateway.do/pay")
async def gateway_pay(body: dict[str, Any]) -> dict[str, object]:
    """AJAX endpoint: mark trade as paid and send signed notification to Platform."""
    out_trade_no = str(body["outTradeNo"])
    trade = _trade(out_trade_no)
    trade["trade_status"] = "TRADE_SUCCESS"
    notify_url = trade.get("_notify_url", "")
    success = await _send_notification_to_platform(
        out_trade_no=out_trade_no,
        trade_status="TRADE_SUCCESS",
        notify_url=notify_url,
    )
    return {"success": success, "error": None if success else "notification_failed"}


@app.post("/gateway.do/close")
async def gateway_close(body: dict[str, Any]) -> dict[str, object]:
    """Mark trade as closed and notify Platform (simulates refund/timeout)."""
    out_trade_no = str(body["outTradeNo"])
    trade = _trade(out_trade_no)
    trade["trade_status"] = "TRADE_CLOSED"
    refund_amount = str(body.get("refundAmount", trade.get("total_amount", "0")))
    if refund_amount:
        trade["refund_amount"] = refund_amount
    notify_url = trade.get("_notify_url", "")
    success = await _send_notification_to_platform(
        out_trade_no=out_trade_no,
        trade_status="TRADE_CLOSED",
        notify_url=notify_url,
        refund_amount=refund_amount if refund_amount != "0" else None,
    )
    return {"success": success}


# ---- Legacy JSON gateway path (kept for E2E test compatibility) ----

def _gateway_json_response(method: str, biz_content: str) -> dict[str, object]:
    payload = json.loads(biz_content)
    out_trade_no = str(payload["out_trade_no"])
    trade = _trade(out_trade_no)
    if method == "alipay.trade.close" and trade["trade_status"] == "WAIT_BUYER_PAY":
        trade["trade_status"] = "TRADE_CLOSED"
    response_key = method.replace(".", "_") + "_response"
    response: dict[str, str] = {"code": "10000", "msg": "Success", **trade}
    raw_response = json.dumps(response, separators=(",", ":"), ensure_ascii=False)
    return {response_key: response, "sign": _sign(raw_response)}

@app.post("/gateway.do/json")
async def gateway_json(method: str = Form(...), biz_content: str = Form(...)) -> dict[str, object]:
    return _gateway_json_response(method, biz_content)

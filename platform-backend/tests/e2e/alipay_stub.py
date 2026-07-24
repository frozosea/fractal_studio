"""Local RSA2-capable Alipay contract stub. Development and E2E only."""

from __future__ import annotations

import base64
import json
from decimal import Decimal
from typing import Any

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from fastapi import FastAPI, Form, HTTPException
from fastapi.responses import PlainTextResponse


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


@app.post("/gateway.do")
async def gateway(method: str = Form(...), biz_content: str = Form(...)) -> dict[str, object]:
    payload = json.loads(biz_content)
    out_trade_no = str(payload["out_trade_no"])
    trade = _trade(out_trade_no)
    if method == "alipay.trade.close" and trade["trade_status"] == "WAIT_BUYER_PAY":
        trade["trade_status"] = "TRADE_CLOSED"
    response_key = method.replace(".", "_") + "_response"
    response: dict[str, str] = {"code": "10000", "msg": "Success", **trade}
    raw_response = json.dumps(response, separators=(",", ":"), ensure_ascii=False)
    return {response_key: response, "sign": _sign(raw_response)}

"""Alipay RSA2 form, notification and reconciliation gateway."""

from __future__ import annotations

import base64
import hashlib
import json
from dataclasses import dataclass
from datetime import UTC, datetime
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any, Literal

import httpx
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa

from app.core.config import Settings, get_settings


@dataclass(frozen=True, slots=True)
class PaymentFormDescriptor:
    action: str
    method: Literal["POST"]
    fields: dict[str, str]


@dataclass(frozen=True, slots=True)
class AlipayTrade:
    out_trade_no: str
    trade_no: str
    trade_status: str
    total_amount: Decimal
    refund_amount: Decimal | None = None


class PaymentGatewayConfigurationError(RuntimeError):
    """Payment operation cannot be safely configured."""


class AlipayProtocolError(RuntimeError):
    """Provider response is malformed, unsigned or inconsistent."""


class AlipayPaymentGateway:
    def __init__(self, settings: Settings | None = None) -> None:
        self._settings = settings or get_settings()
        self._public_key: rsa.RSAPublicKey | None = None

    def start_payment(
        self, *, out_trade_no: str, amount: Decimal, subject: str, channel: Literal["desktop_web", "mobile_web"]
    ) -> PaymentFormDescriptor:
        method, product_code = (
            ("alipay.trade.page.pay", "FAST_INSTANT_TRADE_PAY")
            if channel == "desktop_web"
            else ("alipay.trade.wap.pay", "QUICK_WAP_WAY")
        )
        if self._settings.alipay_stub_mode:
            return self._stub_form(
                method=method, product_code=product_code, out_trade_no=out_trade_no, amount=amount, subject=subject
            )
        fields = self._request_fields(
            method=method,
            biz_content={
                "out_trade_no": out_trade_no,
                "product_code": product_code,
                "total_amount": self._money(amount),
                "subject": self._subject(subject),
                "seller_id": self._settings.alipay_seller_id,
            },
            include_return_urls=True,
        )
        return PaymentFormDescriptor(action=self._settings.alipay_gateway_url, method="POST", fields=fields)

    async def verify_notification(self, fields: dict[str, str]) -> None:
        """Verify signed form fields; caller validates business/order fields under DB lock."""
        sign = fields.get("sign", "")
        if fields.get("sign_type", "").upper() != "RSA2" or not sign:
            raise AlipayProtocolError("alipay_signature_invalid")
        charset = self._charset(fields.get("charset", "utf-8"))
        try:
            signature = base64.b64decode(sign, validate=True)
            (await self._provider_public_key()).verify(
                signature, self._canonical(fields).encode(charset), padding.PKCS1v15(), hashes.SHA256()
            )
        except (InvalidSignature, ValueError, UnicodeError) as error:
            raise AlipayProtocolError("alipay_signature_invalid") from error

    async def query_trade(self, *, out_trade_no: str) -> AlipayTrade:
        payload = await self._call(method="alipay.trade.query", biz_content={"out_trade_no": out_trade_no})
        return self._trade(payload, expected_out_trade_no=out_trade_no)

    async def close_trade(self, *, out_trade_no: str) -> AlipayTrade | None:
        payload = await self._call(method="alipay.trade.close", biz_content={"out_trade_no": out_trade_no})
        if payload.get("code") != "10000":
            return None
        return self._trade(payload, expected_out_trade_no=out_trade_no)

    async def _call(self, *, method: str, biz_content: dict[str, object]) -> dict[str, Any]:
        fields = self._request_fields(method=method, biz_content=biz_content, include_return_urls=False)
        try:
            async with httpx.AsyncClient(timeout=self._settings.alipay_timeout_seconds, trust_env=False) as client:
                response = await client.post(self._settings.alipay_gateway_url, data=fields)
                response.raise_for_status()
        except httpx.HTTPError as error:
            raise AlipayProtocolError("alipay_gateway_unavailable") from error
        return await self._verified_gateway_response(response.text, method=method)

    def _request_fields(
        self, *, method: str, biz_content: dict[str, object], include_return_urls: bool
    ) -> dict[str, str]:
        settings = self._settings
        if not all((settings.alipay_app_id, settings.alipay_seller_id)) and not settings.alipay_stub_mode:
            raise PaymentGatewayConfigurationError("alipay_configuration_incomplete")
        fields = {
            "app_id": settings.alipay_app_id or "dev-stub",
            "method": method,
            "format": "JSON",
            "charset": "utf-8",
            "sign_type": "RSA2",
            "timestamp": datetime.now(UTC).astimezone().strftime("%Y-%m-%d %H:%M:%S"),
            "version": "1.0",
            "biz_content": json.dumps(biz_content, separators=(",", ":"), ensure_ascii=False),
        }
        if include_return_urls:
            if not all((settings.alipay_notify_url, settings.alipay_return_url)) and not settings.alipay_stub_mode:
                raise PaymentGatewayConfigurationError("alipay_configuration_incomplete")
            fields["notify_url"] = settings.alipay_notify_url
            fields["return_url"] = settings.alipay_return_url
        if settings.alipay_stub_mode:
            fields["sign"] = "dev-stub-" + hashlib.sha256(self._canonical(fields).encode()).hexdigest()
            return fields
        fields["sign"] = self._sign(self._canonical(fields).encode())
        return fields

    async def _verified_gateway_response(self, body: str, *, method: str) -> dict[str, Any]:
        key = method.replace(".", "_") + "_response"
        try:
            parsed = json.loads(body)
            signed_payload = self._raw_top_level_value(body, key)
            signature = base64.b64decode(str(parsed["sign"]), validate=True)
            (await self._provider_public_key()).verify(
                signature, signed_payload.encode("utf-8"), padding.PKCS1v15(), hashes.SHA256()
            )
            payload = parsed[key]
        except (KeyError, ValueError, TypeError, InvalidSignature) as error:
            raise AlipayProtocolError("alipay_response_signature_invalid") from error
        if not isinstance(payload, dict):
            raise AlipayProtocolError("alipay_response_invalid")
        return payload

    async def _provider_public_key(self) -> rsa.RSAPublicKey:
        if self._public_key is not None:
            return self._public_key
        if self._settings.alipay_stub_mode:
            url = self._settings.alipay_stub_public_key_url
            if not url:
                raise PaymentGatewayConfigurationError("alipay_stub_public_key_unavailable")
            try:
                async with httpx.AsyncClient(timeout=self._settings.alipay_timeout_seconds, trust_env=False) as client:
                    content = (await client.get(url)).content
            except httpx.HTTPError as error:
                raise PaymentGatewayConfigurationError("alipay_stub_public_key_unavailable") from error
        else:
            key_path = Path(self._settings.alipay_public_key_path)
            if not key_path.is_file():
                raise PaymentGatewayConfigurationError("alipay_public_key_unavailable")
            try:
                content = key_path.read_bytes()
            except OSError as error:
                raise PaymentGatewayConfigurationError("alipay_public_key_unavailable") from error
        try:
            normalized = self._pem_public_key(content)
            key = serialization.load_pem_public_key(normalized)
        except (TypeError, ValueError) as error:
            raise PaymentGatewayConfigurationError("alipay_public_key_invalid") from error
        if not isinstance(key, rsa.RSAPublicKey):
            raise PaymentGatewayConfigurationError("alipay_public_key_invalid")
        self._public_key = key
        return key

    def _sign(self, content: bytes) -> str:
        key_path = Path(self._settings.alipay_private_key_path)
        if not key_path.is_file():
            raise PaymentGatewayConfigurationError("alipay_private_key_unavailable")
        try:
            key = serialization.load_pem_private_key(key_path.read_bytes(), password=None)
            signature = key.sign(content, padding.PKCS1v15(), hashes.SHA256())
        except (OSError, TypeError, ValueError) as error:
            raise PaymentGatewayConfigurationError("alipay_private_key_invalid") from error
        return base64.b64encode(signature).decode()

    def _stub_form(
        self, *, method: str, product_code: str, out_trade_no: str, amount: Decimal, subject: str
    ) -> PaymentFormDescriptor:
        fields = self._request_fields(method=method, biz_content={
            "out_trade_no": out_trade_no, "product_code": product_code, "total_amount": self._money(amount),
            "subject": self._subject(subject), "seller_id": "dev-stub",
        }, include_return_urls=True)
        return PaymentFormDescriptor(action=self._settings.alipay_gateway_url, method="POST", fields=fields)

    @staticmethod
    def _trade(payload: dict[str, Any], *, expected_out_trade_no: str) -> AlipayTrade:
        try:
            amount = Decimal(str(payload["total_amount"])).quantize(Decimal("0.01"))
            refund_raw = payload.get("refund_amount")
            refund_amount = Decimal(str(refund_raw)).quantize(Decimal("0.01")) if refund_raw is not None else None
            trade = AlipayTrade(out_trade_no=str(payload["out_trade_no"]), trade_no=str(payload["trade_no"]),
                trade_status=str(payload["trade_status"]), total_amount=amount, refund_amount=refund_amount)
        except (KeyError, InvalidOperation, ValueError) as error:
            raise AlipayProtocolError("alipay_trade_response_invalid") from error
        if trade.out_trade_no != expected_out_trade_no or not trade.trade_no or amount <= 0:
            raise AlipayProtocolError("alipay_trade_response_invalid")
        return trade

    @staticmethod
    def _raw_top_level_value(body: str, key: str) -> str:
        marker = json.dumps(key) + ":"
        start = body.find(marker)
        if start < 0:
            raise ValueError("response node missing")
        value_start = start + len(marker)
        while value_start < len(body) and body[value_start].isspace():
            value_start += 1
        _, end = json.JSONDecoder().raw_decode(body[value_start:])
        return body[value_start:value_start + end]

    @staticmethod
    def _canonical(fields: dict[str, str]) -> str:
        return "&".join(f"{key}={fields[key]}" for key in sorted(fields) if key not in {"sign", "sign_type"} and fields[key] != "")

    @staticmethod
    def _charset(value: str) -> str:
        normalized = value.lower().replace("_", "-")
        return {"utf-8": "utf-8", "gbk": "gbk", "gb2312": "gbk"}.get(normalized, "utf-8")

    @staticmethod
    def _pem_public_key(content: bytes) -> bytes:
        stripped = content.strip()
        if b"BEGIN PUBLIC KEY" in stripped:
            return stripped
        return b"-----BEGIN PUBLIC KEY-----\n" + stripped + b"\n-----END PUBLIC KEY-----\n"

    @staticmethod
    def _money(amount: Decimal) -> str:
        if not amount.is_finite() or amount <= 0 or amount.as_tuple().exponent < -2:
            raise PaymentGatewayConfigurationError("alipay_amount_invalid")
        return format(amount.quantize(Decimal("0.01")), ".2f")

    @staticmethod
    def _subject(value: str) -> str:
        subject = value.strip()[:128]
        if not subject:
            raise PaymentGatewayConfigurationError("alipay_subject_invalid")
        return subject

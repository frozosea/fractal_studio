"""M5 Alipay payment-start adapter. Callback verification belongs to T12."""

from __future__ import annotations

import base64
import hashlib
import json
from dataclasses import dataclass
from datetime import UTC, datetime
from decimal import Decimal
from pathlib import Path
from typing import Literal

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding

from app.core.config import Settings, get_settings


@dataclass(frozen=True, slots=True)
class PaymentFormDescriptor:
    action: str
    method: Literal["POST"]
    fields: dict[str, str]


class PaymentGatewayConfigurationError(RuntimeError):
    """Payment start cannot be safely created with current configuration."""


class AlipayPaymentGateway:
    def __init__(self, settings: Settings | None = None) -> None:
        self._settings = settings or get_settings()

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
        fields = self._unsigned_fields(
            method=method, product_code=product_code, out_trade_no=out_trade_no, amount=amount, subject=subject
        )
        private_key_path = Path(self._settings.alipay_private_key_path)
        if not private_key_path.is_file():
            raise PaymentGatewayConfigurationError("alipay_private_key_unavailable")
        try:
            private_key = serialization.load_pem_private_key(private_key_path.read_bytes(), password=None)
            signature = private_key.sign(self._canonical(fields).encode(), padding.PKCS1v15(), hashes.SHA256())
        except (OSError, TypeError, ValueError) as error:
            raise PaymentGatewayConfigurationError("alipay_private_key_invalid") from error
        fields["sign"] = base64.b64encode(signature).decode()
        return PaymentFormDescriptor(action=self._settings.alipay_gateway_url, method="POST", fields=fields)

    def _unsigned_fields(
        self, *, method: str, product_code: str, out_trade_no: str, amount: Decimal, subject: str
    ) -> dict[str, str]:
        settings = self._settings
        if not all((settings.alipay_app_id, settings.alipay_seller_id, settings.alipay_notify_url, settings.alipay_return_url)):
            raise PaymentGatewayConfigurationError("alipay_configuration_incomplete")
        safe_subject = subject.strip()[:128]
        if not safe_subject:
            raise PaymentGatewayConfigurationError("alipay_subject_invalid")
        return {
            "app_id": settings.alipay_app_id,
            "method": method,
            "charset": "utf-8",
            "sign_type": "RSA2",
            "timestamp": datetime.now(UTC).astimezone().strftime("%Y-%m-%d %H:%M:%S"),
            "version": "1.0",
            "notify_url": settings.alipay_notify_url,
            "return_url": settings.alipay_return_url,
            "biz_content": json.dumps({
                "out_trade_no": out_trade_no, "product_code": product_code, "total_amount": format(amount, ".2f"),
                "subject": safe_subject, "seller_id": settings.alipay_seller_id,
            }, separators=(",", ":"), ensure_ascii=False),
        }

    def _stub_form(
        self, *, method: str, product_code: str, out_trade_no: str, amount: Decimal, subject: str
    ) -> PaymentFormDescriptor:
        fields = {
            "app_id": "dev-stub", "method": method, "charset": "utf-8", "sign_type": "RSA2",
            "timestamp": "1970-01-01 00:00:00", "version": "1.0", "notify_url": "http://dev.invalid/alipay/notify",
            "return_url": "http://dev.invalid/alipay/return",
            "biz_content": json.dumps({"out_trade_no": out_trade_no, "product_code": product_code,
                "total_amount": format(amount, ".2f"), "subject": subject.strip()[:128], "seller_id": "dev-stub"},
                separators=(",", ":"), ensure_ascii=False),
        }
        fields["sign"] = "dev-stub-" + hashlib.sha256(self._canonical(fields).encode()).hexdigest()
        return PaymentFormDescriptor(action="https://dev.invalid/alipay/gateway", method="POST", fields=fields)

    @staticmethod
    def _canonical(fields: dict[str, str]) -> str:
        return "&".join(f"{key}={fields[key]}" for key in sorted(fields) if key != "sign" and fields[key] != "")

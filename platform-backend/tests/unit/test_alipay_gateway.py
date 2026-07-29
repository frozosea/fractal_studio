"""Regression tests for the two distinct Alipay RSA2 canonical forms."""

from app.infrastructure.alipay.payment_gateway import AlipayPaymentGateway


def test_request_signature_includes_sign_type() -> None:
    fields = {
        "app_id": "sandbox-app",
        "method": "alipay.trade.query",
        "sign_type": "RSA2",
        "sign": "transport-signature",
    }

    assert AlipayPaymentGateway._canonical(fields) == (
        "app_id=sandbox-app&method=alipay.trade.query&sign_type=RSA2"
    )


def test_notification_signature_excludes_sign_and_sign_type() -> None:
    fields = {
        "app_id": "sandbox-app",
        "out_trade_no": "fs123",
        "sign_type": "RSA2",
        "sign": "transport-signature",
        "trade_status": "TRADE_SUCCESS",
    }

    assert AlipayPaymentGateway._notification_canonical(fields) == (
        "app_id=sandbox-app&out_trade_no=fs123&trade_status=TRADE_SUCCESS"
    )


def test_non_ascii_subject_uses_signature_stable_provider_label() -> None:
    assert AlipayPaymentGateway._subject("哦哦哦") == "Fractal Studio order"
    assert AlipayPaymentGateway._subject("Fractal artwork") == "Fractal artwork"

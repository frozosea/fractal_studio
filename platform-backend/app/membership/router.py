"""Membership: pay-once lifetime upgrade. Checkout→Alipay→settlement→auto-grant."""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from decimal import Decimal
from uuid import UUID, uuid4

from fastapi import APIRouter, Depends, Header, Request, Response
from fastapi.responses import JSONResponse
from sqlalchemy import text

from app.auth.models import AccessPrincipal
from app.commerce import repository as commerce_repo
from app.commerce.models import AlipayFormView, OrderItemView, OrderView, PaymentAttemptView, PaymentStartView
from app.core import audit_writer, idempotency_service
from app.core.access_middleware import enforce_origin_and_csrf, require_principal, require_role
from app.core.config import get_settings
from app.core.db import get_engine
from app.core.request_context import request_id
from app.infrastructure.alipay.payment_gateway import AlipayPaymentGateway, PaymentGatewayConfigurationError
from app.outbox.models import NewOutboxEvent
from app.outbox.service import TransactionalOutboxService

router = APIRouter(prefix="/v1", tags=["membership"])

MEMBERSHIP_PRICE = Decimal("29.00")
MEMBERSHIP_POLICY = "membership-v1"
_CENT = Decimal("0.01")


@router.get("/me/membership")
async def my_membership(principal: AccessPrincipal = Depends(require_principal)) -> dict[str, object]:
    async with get_engine().connect() as c:
        row = await c.execute(
            text("SELECT status FROM memberships WHERE user_id = :uid"),
            {"uid": principal.user_id},
        )
        member = row.scalar()
    return {"data": {"member": member == "active"}}


@router.post("/membership/checkout")
async def membership_checkout(
    request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_principal),
) -> Response:
    """Create a membership order and return an Alipay payment form."""
    enforce_origin_and_csrf(request, principal)
    settings = get_settings()
    payments = AlipayPaymentGateway()
    now = datetime.now(UTC)
    expires_at = now + timedelta(minutes=settings.payment_attempt_ttl_minutes)
    out_trade_no = f"fs{uuid4().hex}"

    try:
        async with get_engine().begin() as connection:
            claim = await idempotency_service.claim(
                connection, user_id=principal.user_id, scope="membership.checkout",
                key=idempotency_key, body={"price": str(MEMBERSHIP_PRICE)},
            )
            if claim.is_replay:
                return JSONResponse(
                    content=claim.replay_body or {},
                    status_code=claim.replay_status or 201,
                    headers=dict(claim.replay_headers or {}),
                )

            # Check if user is already a member
            already = await connection.scalar(
                text("SELECT 1 FROM memberships WHERE user_id = :uid AND status = 'active'"),
                {"uid": principal.user_id},
            )
            if already:
                return JSONResponse(status_code=409, content={
                    "error": {"code": "already_member", "message": "already a member", "details": {}}
                })

            # Check no pending membership order
            pending = await connection.scalar(
                text("""
                    SELECT o.id FROM orders o
                    JOIN order_items oi ON oi.order_id = o.id
                    WHERE o.buyer_id = :uid AND oi.commission_policy_version = :pol
                      AND o.status = 'pending_payment'
                    LIMIT 1
                """),
                {"uid": principal.user_id, "pol": MEMBERSHIP_POLICY},
            )
            if pending:
                return JSONResponse(status_code=409, content={
                    "error": {"code": "payment_already_pending", "message": "pending membership payment exists", "details": {}}
                })

            order_id, attempt_id = uuid4(), uuid4()
            await connection.execute(text("""
                INSERT INTO orders (id, buyer_id, status, amount, currency)
                VALUES (:id, :buyer_id, 'pending_payment', :amount, 'CNY')
            """), {"id": order_id, "buyer_id": principal.user_id, "amount": MEMBERSHIP_PRICE})
            await connection.execute(text("""
                INSERT INTO payment_attempts (id, order_id, out_trade_no, status, amount, expires_at)
                VALUES (:id, :order_id, :out_trade_no, 'created', :amount, :expires_at)
            """), {"id": attempt_id, "order_id": order_id, "out_trade_no": out_trade_no,
                 "amount": MEMBERSHIP_PRICE, "expires_at": expires_at})
            # Store membership intent separately from the marketplace order-item chain.
            await connection.execute(text("""
                INSERT INTO membership_orders (order_id, user_id) VALUES (:oid, :uid)
            """), {"oid": order_id, "uid": principal.user_id})

            form = payments.start_payment(
                out_trade_no=out_trade_no, amount=MEMBERSHIP_PRICE,
                subject="Fractal Studio Lifetime Membership", channel="desktop_web",
            )
            view = PaymentStartView(
                order=OrderView(
                    id=order_id, status="pending_payment", amount=MEMBERSHIP_PRICE, currency="CNY",
                    paidAt=None, createdAt=now, items=[],
                ),
                paymentAttempt=PaymentAttemptView(
                    id=attempt_id, outTradeNo=out_trade_no, status="created", expiresAt=expires_at,
                ),
                alipayForm=AlipayFormView(action=form.action, method=form.method, fields=form.fields),
            )
            body: dict[str, object] = {"data": view.model_dump(mode="json", by_alias=True)}
            headers = {"Cache-Control": "no-store"}

            await TransactionalOutboxService(connection).append(NewOutboxEvent(
                event_type="payment.reconcile.v1", aggregate_type="order", aggregate_id=order_id,
                idempotency_key="membership-initial",
                payload={"orderId": str(order_id), "paymentAttemptId": str(attempt_id),
                         "outTradeNo": out_trade_no},
                available_at=now + timedelta(seconds=settings.payment_reconcile_delay_seconds),
                causation_request_id=request_id(request),
            ))
            await audit_writer.record_user_action(
                connection, actor_user_id=principal.user_id, action="membership.checkout_created",
                subject_type="order", subject_id=order_id, request_id_value=request_id(request),
            )
            await idempotency_service.complete(connection, claim, response_status=201,
                                               response_body=body, response_headers=headers)
    except PaymentGatewayConfigurationError as error:
        from fastapi import HTTPException, status
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=str(error)) from error

    return JSONResponse(content=body, status_code=201, headers=headers)


@router.post("/internal/membership/grant")
async def grant_membership(
    request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_role("finance_operator")),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body_raw = await request.json()
    target_email = str(body_raw.get("email", ""))
    async with get_engine().begin() as c:
        uid = await c.scalar(text("SELECT id FROM users WHERE email = :e"), {"e": target_email})
        if uid is None:
            return JSONResponse(status_code=404, content={"error": {"code": "user_not_found", "message": "user not found", "details": {}}})
        await c.execute(
            text("INSERT INTO memberships (user_id, status, granted_by) VALUES (:uid, 'active', :op) ON CONFLICT (user_id) DO UPDATE SET status = 'active', granted_by = :op, granted_at = now()"),
            {"uid": uid, "op": principal.user_id},
        )
    return JSONResponse(status_code=200, content={"data": {"member": True}})

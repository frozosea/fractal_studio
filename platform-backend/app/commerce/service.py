"""M5 checkout orchestration. Provider callbacks and settlement start in T12."""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from decimal import Decimal, ROUND_HALF_UP
from typing import cast
from uuid import UUID, uuid4

from fastapi import HTTPException, Request, status

from app.auth.models import AccessPrincipal
from app.commerce import repository
from app.commerce.models import AlipayFormView, OrderItemView, OrderView, PaymentAttemptView, PaymentStartView
from app.core import audit_writer, idempotency_service
from app.core.config import get_settings
from app.core.db import get_engine
from app.core.request_context import request_id
from app.infrastructure.alipay.payment_gateway import AlipayPaymentGateway, PaymentGatewayConfigurationError
from app.marketplace.ports import ListingSnapshotReader, PublishedOfferSnapshot
from app.marketplace.service import MarketplaceService
from app.outbox.models import NewOutboxEvent
from app.outbox.service import TransactionalOutboxService


_CENT = Decimal("0.01")


def _order_view(record: repository.OrderRecord) -> OrderView:
    return OrderView(
        id=record.id, status=cast(object, record.status), amount=record.amount, currency=cast(object, record.currency),
        paidAt=record.paid_at, createdAt=record.created_at,
        items=[OrderItemView(id=item.id, listingId=item.listing_id, listingVersionId=item.listing_version_id,
            licenceOfferId=item.licence_offer_id, assetId=item.asset_id, creatorId=item.creator_id, price=item.price,
            currency=cast(object, item.currency), commissionPolicyVersion=item.commission_policy_version,
            creatorAmount=item.creator_amount, platformFeeAmount=item.platform_fee_amount) for item in record.items],
    )


class CheckoutService:
    def __init__(
        self, *, listings: ListingSnapshotReader | None = None, payments: AlipayPaymentGateway | None = None
    ) -> None:
        self._listings = listings or MarketplaceService()
        self._payments = payments or AlipayPaymentGateway()

    async def checkout(
        self, *, principal: AccessPrincipal, listing_id: UUID, licence_offer_id: UUID, channel: str,
        idempotency_key: str, request: Request,
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        offer = await self._listings.find_published_offer(listing_id=listing_id, licence_offer_id=licence_offer_id)
        if offer is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="published_offer_not_found")
        if offer.currency != "CNY" or offer.price < _CENT:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="published_offer_invalid")
        settings = get_settings()
        now = datetime.now(UTC)
        expires_at = now + timedelta(minutes=settings.payment_attempt_ttl_minutes)
        out_trade_no = f"fs{uuid4().hex}"
        creator_amount, platform_fee_amount = self._split(offer.price)
        try:
            async with get_engine().begin() as connection:
                claim = await idempotency_service.claim(
                    connection, user_id=principal.user_id, scope="commerce.checkout", key=idempotency_key,
                    body={"listingId": str(listing_id), "licenceOfferId": str(licence_offer_id), "channel": channel},
                )
                if claim.is_replay:
                    return claim.replay_body or {}, claim.replay_status or 201, claim.replay_headers or {}
                active = await repository.find_active_order_for_listing(
                    connection, buyer_id=principal.user_id, listing_id=listing_id
                )
                if active is not None:
                    raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="payment_already_pending")
                order, attempt = await repository.create_pending_order(
                    connection, buyer_id=principal.user_id, offer=offer,
                    commission_policy_version=settings.commission_policy_version, creator_amount=creator_amount,
                    platform_fee_amount=platform_fee_amount, out_trade_no=out_trade_no, expires_at=expires_at,
                )
                form = self._payments.start_payment(
                    out_trade_no=attempt.out_trade_no, amount=order.amount, subject=self._subject(offer), channel=cast(object, channel)
                )
                view = PaymentStartView(order=_order_view(order), paymentAttempt=PaymentAttemptView(
                    id=attempt.id, outTradeNo=attempt.out_trade_no, status=cast(object, attempt.status), expiresAt=attempt.expires_at
                ), alipayForm=AlipayFormView(action=form.action, method=form.method, fields=form.fields))
                body: dict[str, object] = {"data": view.model_dump(mode="json", by_alias=True)}
                headers = {"Cache-Control": "no-store"}
                await TransactionalOutboxService(connection).append(NewOutboxEvent(
                    event_type="payment.reconcile.v1", aggregate_type="order", aggregate_id=order.id,
                    idempotency_key="initial", payload={"orderId": str(order.id), "paymentAttemptId": str(attempt.id),
                        "outTradeNo": attempt.out_trade_no}, available_at=now + timedelta(seconds=settings.payment_reconcile_delay_seconds),
                    causation_request_id=request_id(request),
                ))
                await audit_writer.record_user_action(
                    connection, actor_user_id=principal.user_id, action="checkout.created", subject_type="order",
                    subject_id=order.id, request_id_value=request_id(request),
                    metadata={"listingId": str(listing_id), "paymentAttemptId": str(attempt.id)},
                )
                await idempotency_service.complete(connection, claim, response_status=201, response_body=body, response_headers=headers)
        except PaymentGatewayConfigurationError as error:
            raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=str(error)) from error
        return body, 201, headers

    async def get_order(self, *, principal: AccessPrincipal, order_id: UUID) -> OrderView:
        async with get_engine().connect() as connection:
            record = await repository.find_order(connection, order_id=order_id, buyer_id=principal.user_id)
        if record is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="order_not_found")
        return _order_view(record)

    async def list_purchases(
        self, *, principal: AccessPrincipal, limit: int, before: tuple[datetime, UUID] | None
    ) -> list[repository.OrderRecord]:
        async with get_engine().connect() as connection:
            return await repository.list_buyer_orders(connection, buyer_id=principal.user_id, limit=limit, before=before)

    @staticmethod
    def _split(price: Decimal) -> tuple[Decimal, Decimal]:
        fee = (price * Decimal(get_settings().platform_fee_bps) / Decimal(10_000)).quantize(_CENT, rounding=ROUND_HALF_UP)
        return price - fee, fee

    @staticmethod
    def _subject(offer: PublishedOfferSnapshot) -> str:
        title = str(offer.listing_snapshot.get("title", "Fractal Studio asset")).strip()
        return (title or "Fractal Studio asset")[:128]

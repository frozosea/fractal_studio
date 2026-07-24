"""M5 checkout orchestration. Provider callbacks and settlement start in T12."""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
import hashlib
import logging
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
from app.infrastructure.alipay.payment_gateway import AlipayProtocolError, AlipayTrade
from app.marketplace.ports import ListingSnapshotReader, PublishedOfferSnapshot
from app.marketplace.service import MarketplaceService
from app.outbox.models import NewOutboxEvent, OutboxEvent, RescheduleOutboxEvent, RetryableOutboxError
from app.outbox.service import TransactionalOutboxService
from app.core.logging import worker_log
from app.finance.models import InsufficientCreatorBalanceError
from app.finance.sale_ledger_writer import PostgresSaleLedgerWriter


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


class CommerceService:
    """T12 authoritative provider transitions. Browser return never calls this service."""

    def __init__(self, *, payments: AlipayPaymentGateway | None = None, ledger: PostgresSaleLedgerWriter | None = None) -> None:
        self._payments = payments or AlipayPaymentGateway()
        self._ledger = ledger or PostgresSaleLedgerWriter()

    async def process_notification(self, *, fields: dict[str, str], request_id_value: str) -> None:
        settings = get_settings()
        self._validate_notification_shape(fields)
        if fields["app_id"] != settings.alipay_app_id or fields["seller_id"] != settings.alipay_seller_id:
            raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="alipay_merchant_mismatch")
        await self._payments.verify_notification(fields)
        trade = self._notification_trade(fields)
        fingerprint = hashlib.sha256(self._notification_canonical(fields).encode()).hexdigest()
        if trade.trade_status == "TRADE_CLOSED":
            await self._process_closed_notification(
                trade=trade, fingerprint=fingerprint, fields=fields, request_id_value=request_id_value
            )
            return
        async with get_engine().begin() as connection:
            attempt = await repository.lock_attempt_by_out_trade_no(connection, out_trade_no=trade.out_trade_no)
            if attempt is None:
                raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="payment_attempt_not_found")
            self._validate_trade_against_attempt(trade, attempt)
            inserted = await repository.insert_payment_notification(
                connection, payment_attempt_id=attempt.id, fingerprint=fingerprint, trade_status=trade.trade_status,
                payload_redacted=self._redacted_notification(fields),
            )
            if not inserted:
                return
            if trade.trade_status in {"TRADE_SUCCESS", "TRADE_FINISHED"}:
                await self._settle_locked(connection, attempt=attempt, trade=trade, request_id_value=request_id_value)

    async def reconcile_event(self, event: OutboxEvent) -> None:
        payment_attempt_id = self._event_attempt_id(event)
        await self.reconcile_pending_payment(payment_attempt_id=payment_attempt_id, request_id_value=event.causation_request_id or f"outbox:{event.id}")

    async def reconcile_pending_payment(self, *, payment_attempt_id: UUID, request_id_value: str) -> None:
        async with get_engine().connect() as connection:
            snapshot = await repository.find_attempt_for_reconciliation(connection, payment_attempt_id=payment_attempt_id)
        if snapshot is None:
            return
        try:
            trade = await self._payments.query_trade(out_trade_no=snapshot.out_trade_no)
        except (AlipayProtocolError, PaymentGatewayConfigurationError) as error:
            raise RetryableOutboxError(str(error)) from error
        if trade.total_amount != snapshot.amount:
            raise RetryableOutboxError("alipay_amount_mismatch")
        if trade.trade_status in {"TRADE_SUCCESS", "TRADE_FINISHED"}:
            await self._settle_by_attempt(payment_attempt_id=payment_attempt_id, trade=trade, request_id_value=request_id_value)
            return
        if trade.trade_status == "TRADE_CLOSED":
            async with get_engine().begin() as connection:
                attempt = await repository.lock_attempt_by_id(connection, payment_attempt_id=payment_attempt_id)
                if attempt is None:
                    return
                self._validate_trade_against_attempt(trade, attempt)
                if attempt.order_status == "pending_payment":
                    if await repository.close_unpaid(connection, attempt=attempt):
                        await self._audit(connection, "payment.closed", attempt, request_id_value)
                    return
            await self._reverse_paid(
                payment_attempt_id=payment_attempt_id, amount=trade.refund_amount or trade.total_amount,
                reference=trade.trade_no, request_id_value=request_id_value,
            )
            return
        if trade.trade_status == "WAIT_BUYER_PAY":
            if snapshot.expires_at <= datetime.now(UTC):
                try:
                    closed = await self._payments.close_trade(out_trade_no=snapshot.out_trade_no)
                except (AlipayProtocolError, PaymentGatewayConfigurationError) as error:
                    raise RetryableOutboxError(str(error)) from error
                if closed is not None and closed.trade_status == "TRADE_CLOSED":
                    async with get_engine().begin() as connection:
                        attempt = await repository.lock_attempt_by_id(connection, payment_attempt_id=payment_attempt_id)
                        if attempt is not None and await repository.close_unpaid(connection, attempt=attempt):
                            await self._audit(connection, "payment.closed", attempt, request_id_value)
                    return
            raise RescheduleOutboxEvent(delay_seconds=get_settings().payment_reconcile_pending_seconds)
        raise RetryableOutboxError("alipay_trade_status_unknown")

    async def schedule_due_work(self, service: TransactionalOutboxService) -> int:
        attempts = await repository.list_pending_attempt_ids(service.connection, limit=100)
        bucket = int(datetime.now(UTC).timestamp() // get_settings().payment_reconcile_sweep_seconds)
        for attempt_id in attempts:
            await service.append(NewOutboxEvent(
                event_type="payment.reconcile.v1", aggregate_type="payment_attempt", aggregate_id=attempt_id,
                idempotency_key=f"sweep-{bucket}", payload={"paymentAttemptId": str(attempt_id)},
            ))
        return len(attempts)

    async def on_reconcile_dead_letter(self, event: OutboxEvent, error_code: str) -> None:
        try:
            payment_attempt_id = self._event_attempt_id(event)
        except RetryableOutboxError:
            worker_log(logging.ERROR, "payment reconcile dead event invalid", event_id=event.id, error_code=error_code)
            return
        async with get_engine().begin() as connection:
            await repository.mark_payment_exception(connection, payment_attempt_id=payment_attempt_id)
        worker_log(logging.ERROR, "payment reconcile requires review", event_id=event.id, error_code=error_code,
                   payment_attempt_id=payment_attempt_id)

    async def _settle_by_attempt(self, *, payment_attempt_id: UUID, trade: AlipayTrade, request_id_value: str) -> None:
        async with get_engine().begin() as connection:
            attempt = await repository.lock_attempt_by_id(connection, payment_attempt_id=payment_attempt_id)
            if attempt is None:
                return
            self._validate_trade_against_attempt(trade, attempt)
            await self._settle_locked(connection, attempt=attempt, trade=trade, request_id_value=request_id_value)

    async def _settle_locked(self, connection, *, attempt: repository.LockedPaymentAttempt, trade: AlipayTrade,
                             request_id_value: str) -> None:
        if attempt.alipay_trade_no is not None and attempt.alipay_trade_no != trade.trade_no:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="alipay_trade_mismatch")
        if not await repository.mark_settled(connection, attempt=attempt, alipay_trade_no=trade.trade_no):
            return
        await repository.grant_entitlements(connection, buyer_id=attempt.buyer_id, items=attempt.items)
        for item in attempt.items:
            await self._ledger.record_sale(connection, order_item_id=item.id, request_id_value=request_id_value)
        await self._audit(connection, "payment.settled", attempt, request_id_value)

    async def _reverse_paid(self, *, payment_attempt_id: UUID, amount: Decimal, reference: str,
                            request_id_value: str) -> None:
        try:
            async with get_engine().begin() as connection:
                attempt = await repository.lock_attempt_by_id(connection, payment_attempt_id=payment_attempt_id)
                if attempt is None or attempt.order_status != "fulfilled":
                    return
                reversal = await repository.create_or_get_reversal(
                    connection, attempt=attempt, amount=amount, external_reference=reference
                )
                if reversal.status in {"applied", "manual_review"}:
                    return
                if amount != attempt.order_amount:
                    await repository.mark_reversal_manual_review(connection, reversal_id=reversal.id, order_id=attempt.order_id)
                    await self._audit(connection, "payment.reversal_manual_review", attempt, request_id_value)
                    return
                for item in attempt.items:
                    await self._ledger.reverse_sale(connection, order_item_id=item.id, request_id_value=request_id_value)
                await repository.apply_reversal(connection, reversal_id=reversal.id, attempt=attempt)
                await self._audit(connection, "payment.reversed", attempt, request_id_value)
        except InsufficientCreatorBalanceError:
            async with get_engine().begin() as connection:
                attempt = await repository.lock_attempt_by_id(connection, payment_attempt_id=payment_attempt_id)
                if attempt is None:
                    return
                reversal = await repository.create_or_get_reversal(
                    connection, attempt=attempt, amount=amount, external_reference=reference
                )
                await repository.mark_reversal_manual_review(connection, reversal_id=reversal.id, order_id=attempt.order_id)
                await self._audit(connection, "payment.reversal_manual_review", attempt, request_id_value)

    async def _process_closed_notification(
        self, *, trade: AlipayTrade, fingerprint: str, fields: dict[str, str], request_id_value: str
    ) -> None:
        """Keep provider acknowledgement coupled to close/reversal outcome, including manual review."""
        try:
            async with get_engine().begin() as connection:
                attempt = await repository.lock_attempt_by_out_trade_no(connection, out_trade_no=trade.out_trade_no)
                if attempt is None:
                    raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="payment_attempt_not_found")
                self._validate_trade_against_attempt(trade, attempt)
                if not await repository.insert_payment_notification(
                    connection, payment_attempt_id=attempt.id, fingerprint=fingerprint, trade_status=trade.trade_status,
                    payload_redacted=self._redacted_notification(fields),
                ):
                    return
                if attempt.order_status == "pending_payment":
                    if await repository.close_unpaid(connection, attempt=attempt):
                        await self._audit(connection, "payment.closed", attempt, request_id_value)
                    return
                if attempt.order_status != "fulfilled":
                    return
                reversal = await repository.create_or_get_reversal(
                    connection, attempt=attempt, amount=trade.refund_amount or trade.total_amount,
                    external_reference=trade.trade_no,
                )
                if reversal.status != "detected":
                    return
                if reversal.amount != attempt.order_amount:
                    await repository.mark_reversal_manual_review(connection, reversal_id=reversal.id, order_id=attempt.order_id)
                    await self._audit(connection, "payment.reversal_manual_review", attempt, request_id_value)
                    return
                for item in attempt.items:
                    await self._ledger.reverse_sale(connection, order_item_id=item.id, request_id_value=request_id_value)
                await repository.apply_reversal(connection, reversal_id=reversal.id, attempt=attempt)
                await self._audit(connection, "payment.reversed", attempt, request_id_value)
        except InsufficientCreatorBalanceError:
            async with get_engine().begin() as connection:
                attempt = await repository.lock_attempt_by_out_trade_no(connection, out_trade_no=trade.out_trade_no)
                if attempt is None:
                    raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="payment_attempt_not_found")
                self._validate_trade_against_attempt(trade, attempt)
                inserted = await repository.insert_payment_notification(
                    connection, payment_attempt_id=attempt.id, fingerprint=fingerprint, trade_status=trade.trade_status,
                    payload_redacted=self._redacted_notification(fields),
                )
                if not inserted:
                    return
                reversal = await repository.create_or_get_reversal(
                    connection, attempt=attempt, amount=trade.refund_amount or trade.total_amount,
                    external_reference=trade.trade_no,
                )
                await repository.mark_reversal_manual_review(connection, reversal_id=reversal.id, order_id=attempt.order_id)
                await self._audit(connection, "payment.reversal_manual_review", attempt, request_id_value)

    @staticmethod
    def _validate_notification_shape(fields: dict[str, str]) -> None:
        required = ("app_id", "seller_id", "out_trade_no", "total_amount", "trade_no", "trade_status", "sign", "sign_type")
        if any(not fields.get(name, "").strip() for name in required):
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="alipay_notification_invalid")

    @staticmethod
    def _notification_trade(fields: dict[str, str]) -> AlipayTrade:
        try:
            amount = Decimal(fields["total_amount"])
            if not amount.is_finite() or amount <= 0 or amount.as_tuple().exponent < -2:
                raise ValueError
            refund_value = fields.get("refund_amount")
            refund = Decimal(refund_value) if refund_value else None
        except (InvalidOperation, ValueError) as error:
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="alipay_amount_invalid") from error
        return AlipayTrade(out_trade_no=fields["out_trade_no"], trade_no=fields["trade_no"],
            trade_status=fields["trade_status"], total_amount=amount.quantize(_CENT),
            refund_amount=refund.quantize(_CENT) if refund is not None else None)

    @staticmethod
    def _validate_trade_against_attempt(trade: AlipayTrade, attempt: repository.LockedPaymentAttempt) -> None:
        if trade.out_trade_no != attempt.out_trade_no or trade.total_amount != attempt.amount:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="alipay_payment_mismatch")

    @staticmethod
    def _notification_canonical(fields: dict[str, str]) -> str:
        return "&".join(f"{key}={fields[key]}" for key in sorted(fields) if key not in {"sign", "sign_type"})

    @staticmethod
    def _redacted_notification(fields: dict[str, str]) -> dict[str, object]:
        return {"fieldNames": sorted(fields), "outTradeNo": fields["out_trade_no"], "tradeStatus": fields["trade_status"],
                "tradeNoHash": hashlib.sha256(fields["trade_no"].encode()).hexdigest(),
                "signatureHash": hashlib.sha256(fields["sign"].encode()).hexdigest()}

    @staticmethod
    def _event_attempt_id(event: OutboxEvent) -> UUID:
        try:
            return UUID(str(event.payload["paymentAttemptId"]))
        except (KeyError, TypeError, ValueError) as error:
            raise RetryableOutboxError("payment_reconcile_payload_invalid") from error

    @staticmethod
    async def _audit(connection, action: str, attempt: repository.LockedPaymentAttempt, request_id_value: str) -> None:
        await audit_writer.record_system_action(
            connection, action=action, subject_type="order", subject_id=attempt.order_id,
            request_id_value=request_id_value, metadata={"paymentAttemptId": str(attempt.id)},
        )

"""Private QR staging plus atomic manual creator-settlement transitions."""

from __future__ import annotations

import hashlib
import io
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from decimal import Decimal, InvalidOperation
from uuid import UUID, uuid4

from fastapi import HTTPException, UploadFile, status
from PIL import Image, UnidentifiedImageError
from sqlalchemy.exc import IntegrityError
import zxingcpp

from app.assets.cleanup_service import queue_object_cleanup
from app.auth.models import AccessPrincipal
from app.core import audit_writer, idempotency_service
from app.core.config import Settings, get_settings
from app.core.db import get_engine
from app.finance import repository
from app.finance.cleanup_service import queue_terminal_qr_cleanup
from app.finance.models import (
    CreatorBalanceView,
    InternalPayoutRequestView,
    PayoutRejectInput,
    PayoutRequestRecord,
    PayoutRequestView,
    PayoutSettlementInput,
)
from app.infrastructure.storage.object_storage import ObjectStorage


_CENT = Decimal("0.01")
_MAX_QR_BYTES = 2 * 1024 * 1024
_MAX_QR_PIXELS = 20_000_000


@dataclass(frozen=True, slots=True)
class QrEvidence:
    streamed_sha256: str
    sanitized_bytes: bytes
    media_type: str


def payout_view(record: PayoutRequestRecord) -> PayoutRequestView:
    return PayoutRequestView(
        id=record.id, amount=record.amount, currency=record.currency, status=record.status,
        createdAt=record.created_at, paidAt=record.paid_at, rejectionReason=record.rejection_reason,
    )


class ManualPayoutService:
    def __init__(self, *, storage: ObjectStorage | None = None, settings: Settings | None = None) -> None:
        self._storage = storage or ObjectStorage()
        self._settings = settings or get_settings()

    @staticmethod
    def normalize_amount(raw: str) -> Decimal:
        try:
            value = Decimal(raw.strip())
        except (AttributeError, InvalidOperation) as error:
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_CONTENT, detail="invalid_payout_amount") from error
        if not value.is_finite() or value <= 0 or value.as_tuple().exponent < -2:
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_CONTENT, detail="invalid_payout_amount")
        return value.quantize(_CENT)

    async def validate_qr_upload(self, upload: UploadFile) -> QrEvidence:
        if upload.content_type not in {"image/png", "image/jpeg"}:
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_CONTENT, detail="invalid_qr_image")
        digest = hashlib.sha256()
        pieces: list[bytes] = []
        size = 0
        while chunk := await upload.read(64 * 1024):
            size += len(chunk)
            if size > _MAX_QR_BYTES:
                raise HTTPException(status_code=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE, detail="payout_qr_too_large")
            digest.update(chunk)
            pieces.append(chunk)
        raw = b"".join(pieces)
        if not raw:
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_CONTENT, detail="invalid_qr_image")
        try:
            with Image.open(io.BytesIO(raw)) as probe:
                probe.verify()
            with Image.open(io.BytesIO(raw)) as source:
                source.load()  # decode scan catches truncated/corrupt images before storage
                expected_format = {"image/png": "PNG", "image/jpeg": "JPEG"}[upload.content_type]
                if source.format != expected_format or source.width * source.height > _MAX_QR_PIXELS:
                    raise ValueError
                normalized = source.convert("RGBA" if "A" in source.getbands() else "RGB")
                if not zxingcpp.read_barcodes(normalized):
                    raise ValueError
                output = io.BytesIO()
                normalized.save(output, format="PNG", optimize=True)
        except (UnidentifiedImageError, OSError, ValueError, Image.DecompressionBombError):
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_CONTENT, detail="invalid_qr_image") from None
        sanitized = output.getvalue()
        if not sanitized or len(sanitized) > _MAX_QR_BYTES:
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_CONTENT, detail="invalid_qr_image")
        return QrEvidence(streamed_sha256=digest.hexdigest(), sanitized_bytes=sanitized, media_type="image/png")

    async def create_request(
        self, *, principal: AccessPrincipal, amount: Decimal, evidence: QrEvidence,
        idempotency_key: str, request_id_value: str,
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        object_key = f"private/payout-qr/{uuid4()}.png"
        await self._storage.upload_private_qr(
            object_key=object_key, body=evidence.sanitized_bytes, media_type=evidence.media_type
        )
        cleanup_needed = True
        try:
            async with get_engine().begin() as connection:
                claim = await idempotency_service.claim(
                    connection, user_id=principal.user_id, scope="payout_requests.create", key=idempotency_key,
                    body={"amount": format(amount, ".2f"), "currency": "CNY", "qrSha256": evidence.streamed_sha256},
                )
                if claim.is_replay:
                    body = claim.replay_body or {}
                    response_status = claim.replay_status or status.HTTP_201_CREATED
                    response_headers = claim.replay_headers or {}
                else:
                    await repository.lock_creator_balance(connection, creator_id=principal.user_id)
                    record = await repository.reserve_and_create_payout(
                        connection, creator_id=principal.user_id, amount=amount, qr_object_key=object_key
                    )
                    if record is None:
                        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="insufficient_creator_balance")
                    await repository.append_payout_entry(
                        connection, payout_request_id=record.id, creator_id=record.creator_id,
                        account="creator_reserved", signed_amount=record.amount, entry_type="payout_reserved",
                    )
                    await audit_writer.record_user_action(
                        connection, actor_user_id=principal.user_id, action="payout.request_created",
                        subject_type="payout_request", subject_id=record.id, request_id_value=request_id_value,
                        metadata={"amount": format(record.amount, ".2f"), "currency": "CNY"},
                    )
                    body = {"data": payout_view(record).model_dump(mode="json", by_alias=True)}
                    response_status, response_headers = status.HTTP_201_CREATED, {}
                    await idempotency_service.complete(
                        connection, claim, response_status=response_status, response_body=body,
                        response_headers=response_headers,
                    )
                    cleanup_needed = False
        except IntegrityError as error:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="payout_request_pending") from error
        finally:
            if cleanup_needed:
                async with get_engine().begin() as connection:
                    await queue_object_cleanup(connection, object_keys=[object_key], causation_request_id=request_id_value)
        return body, response_status, response_headers

    async def list_creator(
        self, *, principal: AccessPrincipal, limit: int, before: tuple[datetime, UUID] | None
    ) -> list[PayoutRequestRecord]:
        async with get_engine().connect() as connection:
            return await repository.find_creator_payouts(
                connection, creator_id=principal.user_id, limit=limit,
                before_created_at=before[0] if before else None, before_id=before[1] if before else None,
            )

    async def creator_balance(self, *, principal: AccessPrincipal) -> CreatorBalanceView:
        async with get_engine().connect() as connection:
            balance = await repository.get_creator_balance(connection, creator_id=principal.user_id)
        return CreatorBalanceView(
            availableAmount=balance.available_amount,
            reservedAmount=balance.reserved_amount,
            currency=balance.currency,
        )

    async def cancel_request(
        self, *, principal: AccessPrincipal, payout_request_id: UUID, idempotency_key: str, request_id_value: str,
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        async with get_engine().begin() as connection:
            claim = await idempotency_service.claim(
                connection, user_id=principal.user_id, scope="payout_requests.cancel", key=idempotency_key,
                body={"payoutRequestId": str(payout_request_id)},
            )
            if claim.is_replay:
                return claim.replay_body or {}, claim.replay_status or 200, claim.replay_headers or {}
            current = await repository.lock_payout_request(connection, payout_request_id=payout_request_id)
            if current is None or current.creator_id != principal.user_id:
                raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="payout_request_not_found")
            if current.status != "pending":
                raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="invalid_payout_state")
            await repository.lock_creator_balance(connection, creator_id=current.creator_id)
            record = await repository.cancel_payout(
                connection, payout_request_id=payout_request_id, creator_id=principal.user_id
            )
            if record is None or await repository.release_or_consume_reservation(
                connection, creator_id=current.creator_id, amount=current.amount, release_to_available=True
            ) is None:
                raise RuntimeError("payout_reservation_invariant")
            await repository.append_payout_entry(
                connection, payout_request_id=record.id, creator_id=record.creator_id,
                account="creator_reserved", signed_amount=-record.amount, entry_type="payout_released",
            )
            await queue_terminal_qr_cleanup(connection, payout_request_id=record.id, status="cancelled", request_id_value=request_id_value)
            await audit_writer.record_user_action(
                connection, actor_user_id=principal.user_id, action="payout.cancelled", subject_type="payout_request",
                subject_id=record.id, request_id_value=request_id_value, metadata={"amount": format(record.amount, ".2f")},
            )
            body = {"data": payout_view(record).model_dump(mode="json", by_alias=True)}
            await idempotency_service.complete(connection, claim, response_status=200, response_body=body)
            return body, 200, {}

    async def list_operator(
        self, *, limit: int, payout_status: str | None, before: tuple[datetime, UUID] | None
    ) -> list[InternalPayoutRequestView]:
        async with get_engine().connect() as connection:
            records = await repository.find_operator_payouts(
                connection, status=payout_status, limit=limit, before_created_at=before[0] if before else None,
                before_id=before[1] if before else None,
            )
        expires = datetime.now(UTC) + timedelta(seconds=self._settings.payout_qr_ttl_seconds)
        views: list[InternalPayoutRequestView] = []
        for record in records:
            qr_url = None
            if record.qr_deleted_at is None:
                qr_url = await self._storage.create_signed_get_url(
                    object_key=record.qr_object_key, expires_seconds=self._settings.payout_qr_ttl_seconds
                )
            views.append(InternalPayoutRequestView(
                **payout_view(record).model_dump(by_alias=False),
                creator={"id": str(record.creator_id), "email": record.creator_email, "handle": record.creator_handle},
                qrUrl=qr_url, qrExpiresAt=expires if qr_url else None,
                operator=({"id": str(record.operator_user_id), "email": record.operator_email or ""}
                          if record.operator_user_id else None), externalReference=record.external_reference,
            ))
        return views

    async def mark_paid(
        self, *, principal: AccessPrincipal, payout_request_id: UUID, payload: PayoutSettlementInput,
        idempotency_key: str, request_id_value: str,
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        try:
            external_reference = payload.normalized_reference()
        except ValueError as error:
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail=str(error)) from error
        return await self._settle(
            principal=principal, payout_request_id=payout_request_id, idempotency_key=idempotency_key,
            request_id_value=request_id_value, next_status="paid", detail=external_reference,
        )

    async def reject(
        self, *, principal: AccessPrincipal, payout_request_id: UUID, payload: PayoutRejectInput,
        idempotency_key: str, request_id_value: str,
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        try:
            reason = payload.normalized_reason()
        except ValueError as error:
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail=str(error)) from error
        return await self._settle(
            principal=principal, payout_request_id=payout_request_id, idempotency_key=idempotency_key,
            request_id_value=request_id_value, next_status="rejected", detail=reason,
        )

    async def _settle(
        self, *, principal: AccessPrincipal, payout_request_id: UUID, idempotency_key: str,
        request_id_value: str, next_status: str, detail: str,
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        scope = f"payout_requests.{next_status}"
        field = "externalReference" if next_status == "paid" else "reason"
        async with get_engine().begin() as connection:
            claim = await idempotency_service.claim(
                connection, user_id=principal.user_id, scope=scope, key=idempotency_key,
                body={"payoutRequestId": str(payout_request_id), field: detail},
            )
            if claim.is_replay:
                return claim.replay_body or {}, claim.replay_status or 200, claim.replay_headers or {}
            current = await repository.lock_payout_request(connection, payout_request_id=payout_request_id)
            if current is None:
                raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="payout_request_not_found")
            if current.status != "pending":
                raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="invalid_payout_state")
            await repository.lock_creator_balance(connection, creator_id=current.creator_id)
            record = await repository.settle_payout(
                connection, payout_request_id=payout_request_id, operator_user_id=principal.user_id,
                next_status=next_status, external_reference=detail if next_status == "paid" else None,
                rejection_reason=detail if next_status == "rejected" else None,
            )
            if record is None or await repository.release_or_consume_reservation(
                connection, creator_id=current.creator_id, amount=current.amount,
                release_to_available=next_status == "rejected",
            ) is None:
                raise RuntimeError("payout_reservation_invariant")
            await repository.append_payout_entry(
                connection, payout_request_id=record.id, creator_id=record.creator_id, account="creator_reserved",
                signed_amount=-record.amount, entry_type="payout_paid" if next_status == "paid" else "payout_released",
            )
            await queue_terminal_qr_cleanup(connection, payout_request_id=record.id, status=next_status, request_id_value=request_id_value)
            digest = hashlib.sha256(detail.encode()).hexdigest()
            await audit_writer.record_user_action(
                connection, actor_user_id=principal.user_id, action=f"payout.{next_status}",
                subject_type="payout_request", subject_id=record.id, request_id_value=request_id_value,
                metadata={"amount": format(record.amount, ".2f"), "detailSha256": digest},
            )
            body = {"data": payout_view(record).model_dump(mode="json", by_alias=True)}
            await idempotency_service.complete(connection, claim, response_status=200, response_body=body)
            return body, 200, {}

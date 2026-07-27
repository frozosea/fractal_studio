"""Finance-operator M6b settlement routes; no automatic transfer API exists here."""

from __future__ import annotations

from typing import Literal
from uuid import UUID

from fastapi import APIRouter, Depends, Header, Query, Request, Response
from fastapi.responses import JSONResponse

from app.auth.models import AccessPrincipal
from app.core.access_middleware import enforce_origin_and_csrf, require_role
from app.core.request_context import request_id
from app.finance.manual_payout_service import ManualPayoutService
from app.finance.models import PayoutRejectInput, PayoutSettlementInput
from app.finance.payout_router import _decode_cursor, _encode_cursor


router = APIRouter(prefix="/internal/v1/payout-requests", tags=["finance-internal"])


@router.get("")
async def list_payout_requests(
    payout_status: Literal["pending", "paid", "rejected", "cancelled"] | None = Query(default=None, alias="status"),
    cursor: str | None = Query(default=None, max_length=2048), limit: int = Query(default=24, ge=1, le=100),
    principal: AccessPrincipal = Depends(require_role("finance_operator")),
) -> dict[str, object]:
    records = await ManualPayoutService().list_operator(
        payout_status=payout_status, limit=limit + 1, before=_decode_cursor(cursor)
    )
    page = records[:limit]
    return {"data": [record.model_dump(mode="json", by_alias=True) for record in page],
            "page": {"nextCursor": _encode_cursor(page[-1]) if len(records) > limit else None}}


@router.post("/{payout_request_id}/mark-paid")
async def mark_paid(
    payout_request_id: UUID, payload: PayoutSettlementInput, request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_role("finance_operator")),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await ManualPayoutService().mark_paid(
        principal=principal, payout_request_id=payout_request_id, payload=payload,
        idempotency_key=idempotency_key, request_id_value=request_id(request),
    )
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response


@router.post("/{payout_request_id}/reject")
async def reject(
    payout_request_id: UUID, payload: PayoutRejectInput, request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_role("finance_operator")),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await ManualPayoutService().reject(
        principal=principal, payout_request_id=payout_request_id, payload=payload,
        idempotency_key=idempotency_key, request_id_value=request_id(request),
    )
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response

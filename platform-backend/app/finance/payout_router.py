"""Creator-only M6b payout routes. QR storage details never cross this boundary."""

from __future__ import annotations

import base64
import json
from datetime import datetime
from uuid import UUID

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request, Response, status
from fastapi.responses import JSONResponse
from starlette.datastructures import UploadFile

from app.auth.models import AccessPrincipal
from app.core.access_middleware import enforce_origin_and_csrf, require_role
from app.core.request_context import request_id
from app.finance.manual_payout_service import ManualPayoutService, payout_view


router = APIRouter(prefix="/v1/me/payout-requests", tags=["finance"])


def _decode_cursor(cursor: str | None) -> tuple[datetime, UUID] | None:
    if cursor is None:
        return None
    try:
        value = json.loads(base64.urlsafe_b64decode(cursor + "==="))
        if value.get("v") != 1 or value.get("kind") != "payout_requests":
            raise ValueError
        return datetime.fromisoformat(str(value["after"]["createdAt"])), UUID(str(value["after"]["id"]))
    except (AttributeError, KeyError, TypeError, ValueError, UnicodeDecodeError):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_cursor") from None


def _encode_cursor(record: object) -> str:
    value = {"v": 1, "kind": "payout_requests", "after": {
        "createdAt": getattr(record, "created_at").isoformat(), "id": str(getattr(record, "id")),
    }}
    return base64.urlsafe_b64encode(json.dumps(value, separators=(",", ":")).encode()).decode().rstrip("=")


async def _multipart_payout(request: Request) -> tuple[str, UploadFile]:
    if not request.headers.get("content-type", "").lower().startswith("multipart/form-data"):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_payout_multipart")
    # Let service own documented 2 MiB boundary instead of Starlette's 1 MiB default part cap.
    form = await request.form(max_files=2, max_fields=4, max_part_size=2 * 1024 * 1024 + 1)
    amounts = form.getlist("amount")
    qrs = form.getlist("qrCode")
    allowed = {"amount", "qrCode"}
    if len(amounts) != 1 or len(qrs) != 1 or any(key not in allowed for key, _ in form.multi_items()):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_payout_multipart")
    amount, qr = amounts[0], qrs[0]
    if not isinstance(amount, str) or not isinstance(qr, UploadFile):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_payout_multipart")
    return amount, qr


@router.post("", status_code=status.HTTP_201_CREATED)
async def create_payout_request(
    request: Request, idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_role("creator")),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    raw_amount, upload = await _multipart_payout(request)
    service = ManualPayoutService()
    try:
        evidence = await service.validate_qr_upload(upload)
        body, response_status, headers = await service.create_request(
            principal=principal, amount=service.normalize_amount(raw_amount), evidence=evidence,
            idempotency_key=idempotency_key, request_id_value=request_id(request),
        )
    finally:
        await upload.close()
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response


@router.get("/balance")
async def get_creator_balance(
    principal: AccessPrincipal = Depends(require_role("creator")),
) -> dict[str, object]:
    view = await ManualPayoutService().creator_balance(principal=principal)
    return {"data": view.model_dump(mode="json", by_alias=True)}


@router.get("")
async def list_payout_requests(
    cursor: str | None = Query(default=None, max_length=2048), limit: int = Query(default=24, ge=1, le=100),
    principal: AccessPrincipal = Depends(require_role("creator")),
) -> dict[str, object]:
    records = await ManualPayoutService().list_creator(
        principal=principal, limit=limit + 1, before=_decode_cursor(cursor)
    )
    page = records[:limit]
    return {"data": [payout_view(record).model_dump(mode="json", by_alias=True) for record in page],
            "page": {"nextCursor": _encode_cursor(page[-1]) if len(records) > limit else None}}


@router.post("/{payout_request_id}/cancel")
async def cancel_payout_request(
    payout_request_id: UUID, request: Request, idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_role("creator")),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await ManualPayoutService().cancel_request(
        principal=principal, payout_request_id=payout_request_id, idempotency_key=idempotency_key,
        request_id_value=request_id(request),
    )
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response

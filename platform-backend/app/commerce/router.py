"""M5 checkout and buyer order HTTP routes. Alipay notification starts in T12."""

from __future__ import annotations

import base64
import json
from datetime import datetime
from urllib.parse import parse_qsl
from uuid import UUID

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request, Response, status
from fastapi.responses import JSONResponse

from app.auth.models import AccessPrincipal
from app.commerce.models import CheckoutInput
from app.commerce.service import CheckoutService, _order_view
from app.commerce.service import CommerceService
from app.core.access_middleware import enforce_origin_and_csrf, require_principal
from app.core.request_context import request_id
from app.infrastructure.alipay.payment_gateway import AlipayProtocolError, PaymentGatewayConfigurationError


router = APIRouter(prefix="/v1", tags=["commerce"])


async def _alipay_form(request: Request) -> dict[str, str]:
    if not request.headers.get("content-type", "").lower().startswith("application/x-www-form-urlencoded"):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="alipay_notification_invalid")
    try:
        raw = await request.body()
        ascii_items = parse_qsl(raw.decode("ascii"), keep_blank_values=True, strict_parsing=True, encoding="ascii")
        charset = dict(ascii_items).get("charset", "utf-8").lower().replace("_", "-")
        encoding = {"utf-8": "utf-8", "gbk": "gbk", "gb2312": "gbk"}.get(charset)
        if encoding is None:
            raise ValueError
        items = parse_qsl(raw.decode("ascii"), keep_blank_values=True, strict_parsing=True, encoding=encoding)
    except (UnicodeDecodeError, ValueError):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="alipay_notification_invalid") from None
    if len({key for key, _ in items}) != len(items):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="alipay_notification_invalid")
    return dict(items)


def _decode_cursor(cursor: str | None) -> tuple[datetime, UUID] | None:
    if cursor is None:
        return None
    try:
        value = json.loads(base64.urlsafe_b64decode(cursor + "==="))
        if value.get("v") != 1 or value.get("kind") != "purchases":
            raise ValueError
        return datetime.fromisoformat(str(value["after"]["createdAt"])), UUID(str(value["after"]["id"]))
    except (KeyError, TypeError, ValueError, UnicodeDecodeError):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_cursor") from None


def _encode_cursor(*, created_at: datetime, order_id: UUID) -> str:
    raw = json.dumps({"v": 1, "kind": "purchases", "after": {"createdAt": created_at.isoformat(), "id": str(order_id)}},
                     separators=(",", ":")).encode()
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


@router.post("/checkout", status_code=status.HTTP_201_CREATED)
async def checkout(
    payload: CheckoutInput, request: Request, idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_principal),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await CheckoutService().checkout(
        principal=principal, listing_id=payload.listing_id, licence_offer_id=payload.licence_offer_id,
        channel=payload.channel, idempotency_key=idempotency_key, request=request,
    )
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response


@router.post("/webhooks/alipay", include_in_schema=False)
async def alipay_webhook(request: Request) -> Response:
    fields = await _alipay_form(request)
    try:
        await CommerceService().process_notification(fields=fields, request_id_value=request_id(request))
    except (AlipayProtocolError, PaymentGatewayConfigurationError) as error:
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail=str(error)) from error
    return Response(content="success", media_type="text/plain")


@router.get("/orders/{order_id}")
async def get_order(order_id: UUID, principal: AccessPrincipal = Depends(require_principal)) -> dict[str, object]:
    view = await CheckoutService().get_order(principal=principal, order_id=order_id)
    return {"data": view.model_dump(mode="json", by_alias=True)}


@router.get("/me/purchases")
async def list_purchases(
    cursor: str | None = Query(default=None, max_length=2048), limit: int = Query(default=24, ge=1, le=100),
    principal: AccessPrincipal = Depends(require_principal),
) -> dict[str, object]:
    records = await CheckoutService().list_purchases(principal=principal, limit=limit + 1, before=_decode_cursor(cursor))
    page = records[:limit]
    next_cursor = _encode_cursor(created_at=page[-1].created_at, order_id=page[-1].id) if len(records) > limit else None
    return {"data": [_order_view(record).model_dump(mode="json", by_alias=True) for record in page],
            "page": {"nextCursor": next_cursor}}

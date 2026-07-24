"""M4 public catalogue and authenticated marketplace HTTP routes."""

from __future__ import annotations

import base64
import json
from datetime import datetime
from decimal import Decimal, InvalidOperation
from typing import Literal
from uuid import UUID

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request, Response, status
from fastapi.responses import JSONResponse

from app.auth.models import AccessPrincipal
from app.core.access_middleware import enforce_origin_and_csrf, require_principal, require_role
from app.marketplace.models import ListingCreateInput, ListingUpdateInput
from app.marketplace.service import MarketplaceService


router = APIRouter(prefix="/v1", tags=["marketplace"])


async def _optional_principal(request: Request) -> AccessPrincipal | None:
    if request.cookies.get("fs_session") is None:
        return None
    return await require_principal(request)


def _encode_cursor(value: object) -> str:
    raw = json.dumps(value, separators=(",", ":"), sort_keys=True).encode()
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def _decode_cursor(cursor: str) -> dict[str, object]:
    try:
        value = json.loads(base64.urlsafe_b64decode(cursor + "==="))
    except (ValueError, UnicodeDecodeError):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_cursor") from None
    if not isinstance(value, dict) or value.get("v") != 1 or not isinstance(value.get("filters"), dict):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_cursor")
    return value


def _normalized_filters(
    *, q: str | None, tag: str | None, creator: str | None, media_type: str | None,
    min_price: Decimal | None, max_price: Decimal | None, sort: str,
) -> dict[str, str | None]:
    for amount in (min_price, max_price):
        if amount is not None and (not amount.is_finite() or amount.as_tuple().exponent < -2):
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_price")
    normalized = {
        "q": q.strip().lower() if q and q.strip() else None,
        "tag": tag.strip().lower() if tag and tag.strip() else None,
        "creator": creator.strip().lower() if creator and creator.strip() else None,
        "mediaType": media_type,
        "minPrice": format(min_price, ".2f") if min_price is not None else None,
        "maxPrice": format(max_price, ".2f") if max_price is not None else None,
        "sort": sort,
    }
    if min_price is not None and max_price is not None and min_price > max_price:
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_price_range")
    return normalized


def _explore_after(cursor: str | None, filters: dict[str, str | None]) -> dict[str, object] | None:
    if cursor is None:
        return None
    decoded = _decode_cursor(cursor)
    if decoded["filters"] != filters:
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="cursor_filter_mismatch")
    after = decoded.get("after")
    if not isinstance(after, dict) or not isinstance(after.get("id"), str) or not isinstance(after.get("value"), str):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_cursor")
    try:
        parsed_id = UUID(after["id"])
        sort = str(filters["sort"])
        if sort == "newest":
            value: object = datetime.fromisoformat(after["value"])
        elif sort in {"price_asc", "price_desc"}:
            value = Decimal(after["value"])
        else:
            value = float(after["value"])
    except (ValueError, InvalidOperation):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_cursor") from None
    return {"id": parsed_id, "value": value}


def _next_explore_cursor(*, filters: dict[str, str | None], record: object) -> str:
    cursor_value = getattr(record, "cursor_value")
    if isinstance(cursor_value, datetime):
        value = cursor_value.isoformat()
    else:
        value = str(cursor_value)
    return _encode_cursor({"v": 1, "filters": filters, "after": {"id": str(getattr(record, "id")), "value": value}})


def _decode_time_cursor(cursor: str | None) -> tuple[datetime, UUID] | None:
    if cursor is None:
        return None
    decoded = _decode_cursor(cursor)
    after = decoded.get("after")
    if decoded.get("kind") not in {"favorites", "creator_listings"} or not isinstance(after, dict):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_cursor")
    try:
        return datetime.fromisoformat(str(after["createdAt"])), UUID(str(after["id"]))
    except (ValueError, KeyError):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_cursor") from None


def _encode_time_cursor(*, kind: Literal["favorites", "creator_listings"], created_at: datetime, item_id: UUID) -> str:
    return _encode_cursor({"v": 1, "kind": kind, "filters": {}, "after": {"createdAt": created_at.isoformat(), "id": str(item_id)}})


@router.get("/explore")
async def explore(
    q: str | None = Query(default=None, max_length=200),
    tag: str | None = Query(default=None, max_length=32),
    creator: str | None = Query(default=None, max_length=32),
    media_type: Literal["image", "video", "mesh"] | None = Query(default=None, alias="mediaType"),
    min_price: Decimal | None = Query(default=None, alias="minPrice", ge=Decimal("0.01")),
    max_price: Decimal | None = Query(default=None, alias="maxPrice", ge=Decimal("0.01")),
    sort: Literal["newest", "price_asc", "price_desc", "relevance"] = "newest",
    cursor: str | None = Query(default=None, max_length=2048),
    limit: int = Query(default=24, ge=1, le=100),
) -> dict[str, object]:
    if sort == "relevance" and not (q and q.strip()):
        sort = "newest"
    filters = _normalized_filters(q=q, tag=tag, creator=creator, media_type=media_type, min_price=min_price, max_price=max_price, sort=sort)
    after = _explore_after(cursor, filters)
    views, records = await MarketplaceService().explore(
        q=filters["q"], tag=filters["tag"], creator=filters["creator"], media_type=media_type,
        min_price=min_price, max_price=max_price, sort=sort, after=after, limit=limit + 1,
    )
    page = views[:limit]
    next_cursor = _next_explore_cursor(filters=filters, record=records[limit - 1]) if len(records) > limit else None
    return {"data": [view.model_dump(mode="json", by_alias=True) for view in page], "page": {"nextCursor": next_cursor}}


@router.get("/listings/{listing_id}")
async def get_listing(
    listing_id: UUID, principal: AccessPrincipal | None = Depends(_optional_principal)
) -> dict[str, object]:
    view = await MarketplaceService().get_listing(listing_id=listing_id, principal=principal)
    return {"data": view.model_dump(mode="json", by_alias=True)}


@router.get("/me/listings")
async def list_my_listings(
    listing_status: Literal["draft", "published", "unpublished"] | None = Query(default=None, alias="status"),
    cursor: str | None = Query(default=None, max_length=2048),
    limit: int = Query(default=24, ge=1, le=100),
    principal: AccessPrincipal = Depends(require_role("creator")),
) -> dict[str, object]:
    before = _decode_time_cursor(cursor)
    views, records = await MarketplaceService().list_creator(
        principal=principal, listing_status=listing_status, limit=limit + 1, before=before
    )
    page = views[:limit]
    next_cursor = (
        _encode_time_cursor(kind="creator_listings", created_at=records[limit - 1].created_at, item_id=records[limit - 1].id)
        if len(records) > limit
        else None
    )
    return {"data": [view.model_dump(mode="json", by_alias=True) for view in page], "page": {"nextCursor": next_cursor}}


@router.post("/listings", status_code=status.HTTP_201_CREATED)
async def create_listing(
    payload: ListingCreateInput, request: Request, idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_role("creator")),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await MarketplaceService().create_draft(
        principal=principal, payload=payload, idempotency_key=idempotency_key, request=request
    )
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response


@router.patch("/listings/{listing_id}")
async def update_listing(
    listing_id: UUID, payload: ListingUpdateInput, request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_role("creator")),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await MarketplaceService().update_draft(
        principal=principal, listing_id=listing_id, payload=payload, idempotency_key=idempotency_key, request=request
    )
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response


@router.post("/listings/{listing_id}/publish")
async def publish_listing(
    listing_id: UUID, request: Request, idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_role("creator")),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await MarketplaceService().publish(
        principal=principal, listing_id=listing_id, idempotency_key=idempotency_key, request=request
    )
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response


@router.post("/listings/{listing_id}/unpublish")
async def unpublish_listing(
    listing_id: UUID, request: Request, idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_role("creator")),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await MarketplaceService().unpublish(
        principal=principal, listing_id=listing_id, idempotency_key=idempotency_key, request=request
    )
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response


@router.get("/me/favorites")
async def list_my_favorites(
    cursor: str | None = Query(default=None, max_length=2048),
    limit: int = Query(default=24, ge=1, le=100), principal: AccessPrincipal = Depends(require_principal)
) -> dict[str, object]:
    before = _decode_time_cursor(cursor)
    views = await MarketplaceService().list_favorites(principal=principal, limit=limit + 1, before=before)
    page = views[:limit]
    next_cursor = (
        _encode_time_cursor(kind="favorites", created_at=page[-1].created_at, item_id=page[-1].asset_id)
        if len(views) > limit and page
        else None
    )
    return {"data": [view.model_dump(mode="json", by_alias=True) for view in page], "page": {"nextCursor": next_cursor}}


@router.post("/assets/{asset_id}/favorite", status_code=status.HTTP_201_CREATED)
async def add_favorite(
    asset_id: UUID, request: Request, idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_principal),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await MarketplaceService().save_favorite(
        principal=principal, asset_id=asset_id, idempotency_key=idempotency_key, request=request
    )
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response


@router.delete("/assets/{asset_id}/favorite", status_code=status.HTTP_204_NO_CONTENT)
async def remove_favorite(
    asset_id: UUID, request: Request, idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_principal),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    response_status, headers = await MarketplaceService().remove_favorite(
        principal=principal, asset_id=asset_id, idempotency_key=idempotency_key, request=request
    )
    response = Response(status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response

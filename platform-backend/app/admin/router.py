"""Administrator-only account, marketplace and statistics routes."""

from __future__ import annotations

import base64
import json
from datetime import datetime
from typing import Literal
from uuid import UUID

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request, Response, status
from fastapi.responses import JSONResponse

from app.admin.models import AdminListingModerationInput, AdminUserUpdateInput
from app.admin.service import AdminService
from app.auth.models import AccessPrincipal
from app.core.access_middleware import enforce_origin_and_csrf, require_role


router = APIRouter(prefix="/internal/v1/admin", tags=["admin-internal"])


def _encode_cursor(
    *, kind: str, filters: dict[str, str | None], created_at: datetime, item_id: UUID
) -> str:
    value = {
        "v": 1,
        "kind": kind,
        "filters": filters,
        "after": {"createdAt": created_at.isoformat(), "id": str(item_id)},
    }
    return base64.urlsafe_b64encode(json.dumps(value, separators=(",", ":")).encode()).decode().rstrip("=")


def _decode_cursor(
    cursor: str | None, *, kind: str, filters: dict[str, str | None]
) -> tuple[datetime, UUID] | None:
    if cursor is None:
        return None
    try:
        decoded = json.loads(base64.urlsafe_b64decode(cursor + "==="))
        after = decoded["after"]
        if (
            decoded.get("v") != 1
            or decoded.get("kind") != kind
            or decoded.get("filters") != filters
        ):
            raise ValueError
        return datetime.fromisoformat(after["createdAt"]), UUID(after["id"])
    except (KeyError, TypeError, ValueError, UnicodeDecodeError):
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_CONTENT, detail="invalid_cursor"
        ) from None


@router.get("/statistics")
async def statistics(
    principal: AccessPrincipal = Depends(require_role("admin")),
) -> dict[str, object]:
    view = await AdminService().statistics()
    return {"data": view.model_dump(mode="json", by_alias=True)}


@router.get("/users")
async def list_users(
    q: str | None = Query(default=None, max_length=200),
    user_status: Literal["active", "disabled"] | None = Query(default=None, alias="status"),
    role: Literal["admin", "creator", "finance_operator"] | None = None,
    cursor: str | None = Query(default=None, max_length=2048),
    limit: int = Query(default=50, ge=1, le=100),
    principal: AccessPrincipal = Depends(require_role("admin")),
) -> dict[str, object]:
    normalized_q = q.strip().lower() if q and q.strip() else None
    filters = {"q": normalized_q, "status": user_status, "role": role}
    views, records = await AdminService().list_users(
        q=normalized_q,
        user_status=user_status,
        role=role,
        before=_decode_cursor(cursor, kind="admin_users", filters=filters),
        limit=limit + 1,
    )
    page = views[:limit]
    next_cursor = None
    if len(records) > limit:
        record = records[limit - 1]
        next_cursor = _encode_cursor(
            kind="admin_users",
            filters=filters,
            created_at=record.created_at,
            item_id=record.id,
        )
    return {
        "data": [view.model_dump(mode="json", by_alias=True) for view in page],
        "page": {"nextCursor": next_cursor},
    }


@router.patch("/users/{user_id}")
async def update_user(
    user_id: UUID,
    payload: AdminUserUpdateInput,
    request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_role("admin")),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await AdminService().update_user(
        principal=principal,
        user_id=user_id,
        payload=payload,
        idempotency_key=idempotency_key,
        request=request,
    )
    return JSONResponse(content=body, status_code=response_status, headers=headers)


@router.get("/listings")
async def list_listings(
    q: str | None = Query(default=None, max_length=200),
    listing_status: Literal["draft", "published", "unpublished", "archived"] | None = Query(
        default=None, alias="status"
    ),
    cursor: str | None = Query(default=None, max_length=2048),
    limit: int = Query(default=50, ge=1, le=100),
    principal: AccessPrincipal = Depends(require_role("admin")),
) -> dict[str, object]:
    normalized_q = q.strip().lower() if q and q.strip() else None
    filters = {"q": normalized_q, "status": listing_status}
    views, records = await AdminService().list_listings(
        q=normalized_q,
        listing_status=listing_status,
        before=_decode_cursor(cursor, kind="admin_listings", filters=filters),
        limit=limit + 1,
    )
    page = views[:limit]
    next_cursor = None
    if len(records) > limit:
        record = records[limit - 1]
        next_cursor = _encode_cursor(
            kind="admin_listings",
            filters=filters,
            created_at=record.created_at,
            item_id=record.id,
        )
    return {
        "data": [view.model_dump(mode="json", by_alias=True) for view in page],
        "page": {"nextCursor": next_cursor},
    }


@router.post("/listings/{listing_id}/moderate")
async def moderate_listing(
    listing_id: UUID,
    payload: AdminListingModerationInput,
    request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_role("admin")),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await AdminService().moderate_listing(
        principal=principal,
        listing_id=listing_id,
        payload=payload,
        idempotency_key=idempotency_key,
        request=request,
    )
    return JSONResponse(content=body, status_code=response_status, headers=headers)

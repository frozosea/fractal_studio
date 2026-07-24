"""M3 private-library HTTP boundary. No storage/Compute internals leave these routes."""

from __future__ import annotations

import base64
from datetime import datetime
from typing import Literal
from uuid import UUID

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request, Response, status
from fastapi.responses import JSONResponse

from app.assets import repository
from app.assets.models import AssetView, AssetVisibilityInput
from app.assets.service import AssetLibraryService, asset_view
from app.auth.models import AccessPrincipal
from app.core.access_middleware import enforce_origin_and_csrf, require_principal
from app.core.db import get_engine


router = APIRouter(prefix="/v1", tags=["assets"])


def _decode_cursor(cursor: str | None) -> tuple[datetime, UUID] | None:
    if cursor is None:
        return None
    try:
        raw = base64.urlsafe_b64decode(cursor.encode() + b"===").decode()
        created_at, asset_id = raw.split("|", 1)
        return datetime.fromisoformat(created_at), UUID(asset_id)
    except (ValueError, UnicodeDecodeError):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_cursor") from None


def _encode_cursor(view: AssetView) -> str:
    raw = f"{view.created_at.isoformat()}|{view.id}".encode()
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


@router.get("/me/assets")
async def list_my_assets(
    cursor: str | None = Query(default=None, max_length=512),
    limit: int = Query(default=24, ge=1, le=100),
    asset_status: Literal["processing", "ready", "failed", "deleted"] | None = Query(
        default=None, alias="status"
    ),
    visibility: Literal["private", "hidden"] | None = Query(default=None),
    media_type: Literal["image", "video", "mesh"] | None = Query(default=None, alias="mediaType"),
    principal: AccessPrincipal = Depends(require_principal),
) -> dict[str, object]:
    before = _decode_cursor(cursor)
    async with get_engine().connect() as connection:
        records = await repository.list_owned(
            connection,
            owner_id=principal.user_id,
            limit=limit + 1,
            status=asset_status,
            visibility=visibility,
            media_type=media_type,
            before_created_at=before[0] if before else None,
            before_id=before[1] if before else None,
        )
    page = [asset_view(record) for record in records[:limit]]
    next_cursor = _encode_cursor(page[-1]) if len(records) > limit and page else None
    return {
        "data": [view.model_dump(mode="json", by_alias=True) for view in page],
        "page": {"nextCursor": next_cursor},
    }


@router.get("/me/assets/{asset_id}")
async def get_my_asset(
    asset_id: UUID, principal: AccessPrincipal = Depends(require_principal)
) -> dict[str, object]:
    async with get_engine().connect() as connection:
        record = await repository.find_owned(connection, asset_id=asset_id, owner_id=principal.user_id)
    if record is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="asset_not_found")
    return {"data": asset_view(record).model_dump(mode="json", by_alias=True)}


@router.patch("/me/assets/{asset_id}")
async def update_my_asset(
    asset_id: UUID,
    payload: AssetVisibilityInput,
    request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_principal),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await AssetLibraryService().change_visibility(
        principal=principal,
        asset_id=asset_id,
        visibility=payload.visibility,
        idempotency_key=idempotency_key,
        request=request,
    )
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response


@router.delete("/me/assets/{asset_id}", status_code=status.HTTP_204_NO_CONTENT)
async def delete_my_asset(
    asset_id: UUID,
    request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_principal),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    response_status, headers = await AssetLibraryService().soft_delete(
        principal=principal, asset_id=asset_id, idempotency_key=idempotency_key, request=request
    )
    response = Response(status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response


@router.post("/assets/{asset_id}/download-url")
async def create_download_url(
    asset_id: UUID, principal: AccessPrincipal = Depends(require_principal)
) -> dict[str, object]:
    view = await AssetLibraryService().create_download_url(principal=principal, asset_id=asset_id)
    return {"data": view.model_dump(mode="json", by_alias=True)}

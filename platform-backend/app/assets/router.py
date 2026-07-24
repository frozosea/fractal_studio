"""M3 private-library HTTP boundary. Deliberately excludes object keys and Compute IDs."""

from __future__ import annotations

from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Query

from app.assets import repository
from app.assets.models import AssetFileView, AssetView
from app.auth.models import AccessPrincipal
from app.core.access_middleware import require_principal
from app.core.db import get_engine


router = APIRouter(prefix="/v1", tags=["assets"])


def _view(record: repository.AssetRecord) -> AssetView:
    return AssetView(
        id=record.id,
        recipeId=record.recipe_id,
        mediaType=record.media_type,
        status=record.status,
        visibility=record.visibility,
        createdAt=record.created_at,
        files=[AssetFileView.model_validate(item) for item in record.files],
    )


@router.get("/me/assets")
async def list_my_assets(
    limit: int = Query(default=24, ge=1, le=100),
    principal: AccessPrincipal = Depends(require_principal),
) -> dict[str, object]:
    async with get_engine().connect() as connection:
        records = await repository.list_owned(connection, owner_id=principal.user_id, limit=limit)
    return {"data": [_view(record).model_dump(mode="json", by_alias=True) for record in records], "page": {"nextCursor": None}}


@router.get("/me/assets/{asset_id}")
async def get_my_asset(
    asset_id: UUID, principal: AccessPrincipal = Depends(require_principal)
) -> dict[str, object]:
    async with get_engine().connect() as connection:
        record = await repository.find_owned(connection, asset_id=asset_id, owner_id=principal.user_id)
    if record is None:
        raise HTTPException(status_code=404, detail="not_found")
    return {"data": _view(record).model_dump(mode="json", by_alias=True)}

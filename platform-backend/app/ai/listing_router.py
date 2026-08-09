"""Authenticated JSON boundary for publish-page AI listing copy."""

from __future__ import annotations

from fastapi import APIRouter, Depends, Header, HTTPException, Request

from app.ai.listing_models import ListingCopyInput
from app.ai.listing_provider import ListingProviderUnavailable
from app.ai.listing_service import ListingPreviewUnavailable, create_listing_copy
from app.auth.models import AccessPrincipal
from app.core.access_middleware import enforce_origin_and_csrf, require_role
from app.core.config import get_settings


router = APIRouter(prefix="/v1", tags=["ai"])


@router.post("/ai/listing-copy")
async def listing_copy(
    payload: ListingCopyInput,
    request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_role("creator")),
) -> dict[str, object]:
    enforce_origin_and_csrf(request, principal)
    if not get_settings().ai_enabled:
        raise HTTPException(status_code=503, detail="AI_DISABLED")
    try:
        result = await create_listing_copy(
            owner_id=principal.user_id,
            payload=payload,
            idempotency_key=idempotency_key,
        )
    except (ListingProviderUnavailable, ListingPreviewUnavailable) as error:
        raise HTTPException(status_code=503, detail="AI_PROVIDER_UNAVAILABLE") from error
    return {"data": result}

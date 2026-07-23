"""M2 HTTP routes for immutable recipes and bounded previews."""

from __future__ import annotations

from uuid import UUID

from fastapi import APIRouter, Depends, Header, Query, Request, Response, status
from fastapi.responses import JSONResponse

from app.auth.models import AccessPrincipal
from app.core.access_middleware import enforce_origin_and_csrf, require_principal
from app.studio.models import PreviewInput, RecipeInput, RenderJobCreateInput
from app.studio.preview_service import PreviewService
from app.studio.recipe_service import canonicalize_spec, create_or_reuse, list_recipes
from app.studio.render_job_service import create as create_render_job
from app.studio.render_job_service import get_owned as get_render_job
from app.studio.render_job_service import request_cancel


router = APIRouter(prefix="/v1", tags=["studio"])


@router.post("/recipes")
async def create_recipe(
    payload: RecipeInput,
    request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_principal),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, _replayed, headers = await create_or_reuse(
        owner_id=principal.user_id,
        canonical=canonicalize_spec(payload.canonical_spec),
        idempotency_key=idempotency_key,
        request=request,
    )
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response


@router.get("/me/recipes")
async def get_my_recipes(
    cursor: str | None = Query(default=None, max_length=512),
    limit: int = Query(default=24, ge=1, le=100),
    principal: AccessPrincipal = Depends(require_principal),
) -> dict[str, object]:
    collection = await list_recipes(owner_id=principal.user_id, cursor=cursor, limit=limit)
    return collection.model_dump(mode="json", by_alias=True)


@router.post("/studio/preview", response_class=Response)
async def preview(
    payload: PreviewInput,
    request: Request,
    principal: AccessPrincipal = Depends(require_principal),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    png = await PreviewService().render(
        owner_id=principal.user_id,
        canonical=canonicalize_spec(payload.canonical_spec),
        width=payload.width,
        height=payload.height,
    )
    return Response(
        content=png,
        media_type="image/png",
        status_code=status.HTTP_200_OK,
        headers={"Cache-Control": "no-store"},
    )


@router.post("/render-jobs")
async def create_job(
    payload: RenderJobCreateInput,
    request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_principal),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await create_render_job(
        principal=principal, payload=payload, idempotency_key=idempotency_key, request=request
    )
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response


@router.get("/render-jobs/{render_job_id}")
async def get_job(
    render_job_id: UUID, principal: AccessPrincipal = Depends(require_principal)
) -> dict[str, object]:
    return {"data": (await get_render_job(owner_id=principal.user_id, job_id=render_job_id)).model_dump(mode="json", by_alias=True)}


@router.post("/render-jobs/{render_job_id}/cancel")
async def cancel_job(
    render_job_id: UUID,
    request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_principal),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, response_status, headers = await request_cancel(
        principal=principal, job_id=render_job_id, idempotency_key=idempotency_key, request=request
    )
    response = JSONResponse(content=body, status_code=response_status)
    for name, value in headers.items():
        response.headers[name] = value
    return response

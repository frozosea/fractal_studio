"""M2 HTTP routes for immutable recipes and bounded previews."""

from __future__ import annotations

from uuid import UUID

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request, Response, status
from fastapi.responses import JSONResponse

from app.auth.models import AccessPrincipal
from app.core.access_middleware import enforce_origin_and_csrf, require_principal
from app.core.db import get_engine
from app.infrastructure.compute.compute_client import ComputeClientError
from app.studio.capability_service import studio_capabilities
from app.studio.models import PreviewInput, PreviewJobInput, RecipeInput, RenderJobCreateInput
from app.studio.preview_service import PreviewJobService, PreviewService
from app.studio.quota_service import RenderQuotaService
from app.studio.recipe_service import canonicalize_spec, create_or_reuse, list_recipes
from app.studio.render_job_service import create as create_render_job
from app.studio.render_job_service import get_owned as get_render_job
from app.studio.render_job_service import request_cancel


router = APIRouter(prefix="/v1", tags=["studio"])


@router.get("/studio/capabilities")
async def get_studio_capabilities(
    principal: AccessPrincipal = Depends(require_principal),
) -> dict[str, object]:
    del principal
    try:
        return {"data": await studio_capabilities()}
    except ComputeClientError as error:
        return JSONResponse(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, content={"error": {"code": error.code, "message": "Compute capabilities unavailable"}})


@router.get("/me/export-allowance")
async def get_export_allowance(
    principal: AccessPrincipal = Depends(require_principal),
) -> dict[str, object]:
    async with get_engine().connect() as connection:
        allowance = await RenderQuotaService().allowance(connection, owner_id=principal.user_id)
    return {
        "data": {
            "member": allowance.member,
            "limit": allowance.limit,
            "used": allowance.used,
            "remaining": allowance.remaining,
        }
    }


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


@router.post("/studio/preview-jobs", status_code=status.HTTP_202_ACCEPTED)
async def create_preview_job(
    payload: PreviewJobInput,
    request: Request,
    principal: AccessPrincipal = Depends(require_principal),
) -> dict[str, object]:
    enforce_origin_and_csrf(request, principal)
    job_id, job_status = await PreviewJobService().submit(
        owner_id=principal.user_id, session_id=principal.session_id, channel=payload.channel,
        canonical=canonicalize_spec(payload.canonical_spec), width=payload.width, height=payload.height,
    )
    return {"data": {"id": job_id, "status": job_status}}


@router.get("/studio/preview-jobs/{job_id}")
async def get_preview_job(
    job_id: UUID, principal: AccessPrincipal = Depends(require_principal),
) -> dict[str, object]:
    value = await PreviewJobService().status(
        job_id=str(job_id), owner_id=principal.user_id, session_id=principal.session_id
    )
    if value is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="preview_not_found")
    return {"data": value}


@router.get("/studio/preview-jobs/{job_id}/image", response_class=Response)
async def get_preview_image(
    job_id: UUID, principal: AccessPrincipal = Depends(require_principal),
) -> Response:
    image = await PreviewJobService().image(
        job_id=str(job_id), owner_id=principal.user_id, session_id=principal.session_id
    )
    if image is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="preview_not_ready")
    return Response(content=image, media_type="image/png", headers={"Cache-Control": "private, max-age=60"})


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

from __future__ import annotations

import uuid

from fastapi import APIRouter, Depends, Header, HTTPException, Request, Response, status
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import Settings, get_settings
from app.core.db import session_dependency
from app.infrastructure.compute.compute_client import ComputeClient, ComputeError

from .schemas import DataEnvelope, PreviewInput, RenderJobCreateInput, RenderJobView
from .service import RenderJobService


router = APIRouter(tags=["foundation-studio"])
service = RenderJobService()


def foundation_subject(settings: Settings = Depends(get_settings)) -> uuid.UUID:
    if not settings.foundation_routes_enabled:
        raise HTTPException(status_code=404, detail="not found")
    return settings.foundation_subject_id


@router.post("/v1/studio/preview")
async def preview(
    input: PreviewInput,
    request: Request,
    settings: Settings = Depends(get_settings),
    _: uuid.UUID = Depends(foundation_subject),
) -> Response:
    client = ComputeClient(settings)
    try:
        result = await client.preview(input.kind, input.payload, request.state.request_id)
    except ComputeError as error:
        raise HTTPException(status_code=error.status_code, detail={
            "code": error.code, "message": str(error), "details": error.details,
        }) from error
    finally:
        await client.close()
    headers = {}
    for name in ("X-FSD-Engine", "X-FSD-Scalar", "X-FSD-Width", "X-FSD-Height", "X-FSD-Pixel-Format"):
        if name in result.headers:
            headers[name] = result.headers[name]
    return Response(result.content, result.status_code, headers=headers, media_type=result.headers.get("content-type"))


@router.post("/v1/render-jobs", response_model=DataEnvelope, status_code=status.HTTP_202_ACCEPTED)
async def create_render_job(
    input: RenderJobCreateInput,
    idempotency_key: str = Header(alias="Idempotency-Key", min_length=8, max_length=200),
    owner_id: uuid.UUID = Depends(foundation_subject),
    session: AsyncSession = Depends(session_dependency),
) -> DataEnvelope:
    async with session.begin():
        job = await service.create_job(
            session, owner_id=owner_id, kind=input.kind,
            payload=input.payload, idempotency_key=idempotency_key,
        )
    return DataEnvelope(data=RenderJobView.model_validate(job))


@router.get("/v1/render-jobs/{job_id}", response_model=DataEnvelope)
async def get_render_job(
    job_id: uuid.UUID,
    owner_id: uuid.UUID = Depends(foundation_subject),
    session: AsyncSession = Depends(session_dependency),
) -> DataEnvelope:
    job = await service.get_owned_job(session, owner_id=owner_id, job_id=job_id)
    if job is None:
        raise HTTPException(status_code=404, detail="render job not found")
    return DataEnvelope(data=RenderJobView.model_validate(job))


@router.post("/v1/render-jobs/{job_id}/cancel", response_model=DataEnvelope, status_code=status.HTTP_202_ACCEPTED)
async def cancel_render_job(
    job_id: uuid.UUID,
    idempotency_key: str = Header(alias="Idempotency-Key", min_length=8, max_length=200),
    owner_id: uuid.UUID = Depends(foundation_subject),
    session: AsyncSession = Depends(session_dependency),
) -> DataEnvelope:
    del idempotency_key
    async with session.begin():
        job = await service.request_cancel(session, owner_id=owner_id, job_id=job_id)
    if job is None:
        raise HTTPException(status_code=404, detail="render job not found")
    return DataEnvelope(data=RenderJobView.model_validate(job))


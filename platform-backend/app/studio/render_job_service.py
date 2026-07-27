"""Transactional browser-side render-job creation and cancellation state transitions."""

from __future__ import annotations

from uuid import UUID, uuid4

from fastapi import HTTPException, Request, status
from app.auth.models import AccessPrincipal
from app.core import audit_writer, idempotency_service
from app.core.db import get_engine
from app.core.request_context import request_id
from app.outbox.models import NewOutboxEvent
from app.outbox.service import TransactionalOutboxService
from app.studio import render_job_repository
from app.studio.compute_request_mapper import RENDER_MAPPING_VERSION, map_durable_v1
from app.studio.models import RenderJobCreateInput, RenderJobView
from app.studio.quota_service import RenderQuotaService


def _view(job: render_job_repository.RenderJobRecord) -> RenderJobView:
    return RenderJobView(
        id=job.id,
        recipe_id=job.recipe_id,
        status=job.status,
        progress_percent=job.progress_percent,
        asset_id=job.asset_id,
        error_code=job.error_code,
        created_at=job.created_at,
    )


async def create(
    *, principal: AccessPrincipal, payload: RenderJobCreateInput, idempotency_key: str, request: Request
) -> tuple[dict[str, object], int, dict[str, str]]:
    output_spec = payload.output.model_dump(mode="json", by_alias=True, exclude_none=True)
    async with get_engine().begin() as connection:
        claim = await idempotency_service.claim(
            connection,
            user_id=principal.user_id,
            scope="studio.render_job.create",
            key=idempotency_key,
            body={"recipeId": str(payload.recipe_id), "output": output_spec},
        )
        if claim.is_replay:
            return claim.replay_body or {}, claim.replay_status or status.HTTP_202_ACCEPTED, claim.replay_headers or {}
        recipe = await render_job_repository.find_recipe_owned(
            connection, recipe_id=payload.recipe_id, owner_id=principal.user_id
        )
        if recipe is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="recipe_not_found")
        job_id = uuid4()
        try:
            _route, compute_request = map_durable_v1(
                recipe["canonical_spec"], output_spec=output_spec, client_job_id=job_id  # type: ignore[arg-type]
            )
        except (KeyError, TypeError, ValueError) as error:
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_CONTENT, detail="unsupported_render_output") from error
        job = await render_job_repository.create(
            connection,
            job_id=job_id,
            owner_id=principal.user_id,
            recipe_id=payload.recipe_id,
            output_kind=payload.output.kind,
            output_spec=output_spec,
            mapping_version=RENDER_MAPPING_VERSION,
            compute_request={"route": _route, "body": compute_request},
            idempotency_key=idempotency_key,
        )
        await RenderQuotaService().reserve(connection, owner_id=principal.user_id, job_id=job.id)
        await TransactionalOutboxService(connection).append(
            NewOutboxEvent(
                event_type="render.created.v1",
                aggregate_type="render_job",
                aggregate_id=job.id,
                idempotency_key="created",
                payload={"renderJobId": str(job.id)},
                causation_request_id=request_id(request),
            )
        )
        await audit_writer.record_user_action(
            connection,
            actor_user_id=principal.user_id,
            action="render_job.created",
            subject_type="render_job",
            subject_id=job.id,
            request_id_value=request_id(request),
            metadata={"recipeId": str(job.recipe_id), "outputKind": job.output_kind},
        )
        body: dict[str, object] = {"data": _view(job).model_dump(mode="json", by_alias=True)}
        headers = {"Cache-Control": "no-store"}
        await idempotency_service.complete(
            connection, claim, response_status=status.HTTP_202_ACCEPTED, response_body=body, response_headers=headers
        )
    return body, status.HTTP_202_ACCEPTED, headers


async def get_owned(*, owner_id: UUID, job_id: UUID) -> RenderJobView:
    async with get_engine().connect() as connection:
        job = await render_job_repository.find_owned(connection, job_id=job_id, owner_id=owner_id)
    if job is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="render_job_not_found")
    return _view(job)


async def request_cancel(
    *, principal: AccessPrincipal, job_id: UUID, idempotency_key: str, request: Request
) -> tuple[dict[str, object], int, dict[str, str]]:
    async with get_engine().begin() as connection:
        claim = await idempotency_service.claim(
            connection,
            user_id=principal.user_id,
            scope="studio.render_job.cancel",
            key=idempotency_key,
            body={"renderJobId": str(job_id)},
        )
        if claim.is_replay:
            return claim.replay_body or {}, claim.replay_status or status.HTTP_202_ACCEPTED, claim.replay_headers or {}
        job = await render_job_repository.lock_for_worker(connection, job_id=job_id)
        if job is None or job.owner_id != principal.user_id:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="render_job_not_found")
        if job.status in {"compute_succeeded", "ingesting", "completed", "failed", "cancelled"}:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="invalid_state")
        changed = await render_job_repository.set_status(
            connection,
            job_id=job.id,
            expected={"queued", "submitting", "running"},
            next_status="cancel_requested",
        )
        if not changed:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="invalid_state")
        await TransactionalOutboxService(connection).append(
            NewOutboxEvent(
                event_type="render.cancel_requested.v1",
                aggregate_type="render_job",
                aggregate_id=job.id,
                idempotency_key="cancel",
                payload={"renderJobId": str(job.id)},
                causation_request_id=request_id(request),
            )
        )
        await audit_writer.record_user_action(
            connection,
            actor_user_id=principal.user_id,
            action="render_job.cancel_requested",
            subject_type="render_job",
            subject_id=job.id,
            request_id_value=request_id(request),
        )
        updated = await render_job_repository.lock_for_worker(connection, job_id=job.id)
        assert updated is not None
        body: dict[str, object] = {"data": _view(updated).model_dump(mode="json", by_alias=True)}
        headers = {"Cache-Control": "no-store"}
        await idempotency_service.complete(
            connection, claim, response_status=status.HTTP_202_ACCEPTED, response_body=body, response_headers=headers
        )
    return body, status.HTTP_202_ACCEPTED, headers

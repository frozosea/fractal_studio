import uuid

import pytest

from app.outbox.models import NewOutboxEvent
from app.outbox.service import TransactionalOutboxService


@pytest.mark.asyncio
@pytest.mark.parametrize("payload", [{"password": "nope"}, {"nested": {"serviceKey": "nope"}}])
async def test_outbox_rejects_secrets_before_database_write(payload: dict) -> None:
    service = TransactionalOutboxService(None)  # type: ignore[arg-type]
    with pytest.raises(ValueError, match="forbidden"):
        await service.append(
            NewOutboxEvent(
                event_type="test.redaction.v1",
                aggregate_type="test",
                aggregate_id=uuid.uuid4(),
                idempotency_key="redaction-check",
                payload=payload,
            )
        )


@pytest.mark.asyncio
async def test_outbox_rejects_unversioned_event_before_database_write() -> None:
    service = TransactionalOutboxService(None)  # type: ignore[arg-type]
    with pytest.raises(ValueError, match="event_type"):
        await service.append(
            NewOutboxEvent(
                event_type="render.created",
                aggregate_type="render_job",
                aggregate_id=uuid.uuid4(),
                idempotency_key="version-check",
                payload={"renderJobId": "safe-id"},
            )
        )

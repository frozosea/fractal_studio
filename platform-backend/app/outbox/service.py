"""Transaction-bound outbox writer. Callers must pass their active producer transaction."""

from __future__ import annotations

import re
from typing import Any
from uuid import UUID

from sqlalchemy.ext.asyncio import AsyncConnection

from app.outbox import repository
from app.outbox.models import NewOutboxEvent


_EVENT_TYPE = re.compile(r"^[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)*\.v[1-9][0-9]*$")
_FORBIDDEN_PAYLOAD_KEY = re.compile(
    r"(?:password|session|cookie|token|secret|service.?key|authorization|alipay.*body)", re.IGNORECASE
)


def _validate_payload(value: Any) -> None:
    if isinstance(value, dict):
        for key, nested_value in value.items():
            if _FORBIDDEN_PAYLOAD_KEY.search(str(key)):
                raise ValueError(f"forbidden outbox payload key: {key}")
            _validate_payload(nested_value)
    elif isinstance(value, list):
        for nested_value in value:
            _validate_payload(nested_value)
    elif not isinstance(value, (str, int, float, bool, type(None))):
        raise ValueError("outbox payload must be JSON-compatible immutable input")


class TransactionalOutboxService:
    """Writes only durable metadata. It never calls a queue, Compute or a domain handler."""

    def __init__(self, connection: AsyncConnection) -> None:
        self._connection = connection

    async def append(self, event: NewOutboxEvent) -> UUID:
        if not _EVENT_TYPE.fullmatch(event.event_type):
            raise ValueError("event_type must end with .v<positive schema version>")
        if event.schema_version < 1:
            raise ValueError("schema_version must be positive")
        if not event.aggregate_type or not event.idempotency_key:
            raise ValueError("aggregate_type and idempotency_key are required")
        _validate_payload(event.payload)
        return await repository.append(self._connection, event)

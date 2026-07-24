"""One safe correlation-aware logging shape for HTTP and workers."""

from __future__ import annotations

import logging
import json
from datetime import UTC, datetime
from typing import Any

from app.core.request_context import idempotency_key_var, request_id_var, user_id_var


class _PipeFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        now = datetime.now(UTC)
        timestamp = now.strftime("%Y-%m-%d %H:%M:%S.") + f"{now.microsecond // 1000:03d} UTC"
        request_id = getattr(record, "request_id", request_id_var.get() or "-")
        idempotency_key = getattr(record, "idempotency_key", idempotency_key_var.get() or "-")
        user_id = getattr(record, "user_id", user_id_var.get() or "-")
        return f"{timestamp} | {request_id} | {idempotency_key} | {user_id} | {record.levelname} | {record.getMessage()}"


class _JsonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        return json.dumps(
            {
                "timestamp_utc": datetime.now(UTC).isoformat(timespec="milliseconds"),
                "request_id": getattr(record, "request_id", request_id_var.get() or "-"),
                "idempotency_key": getattr(record, "idempotency_key", idempotency_key_var.get() or "-"),
                "user_id": getattr(record, "user_id", user_id_var.get() or "-"),
                "level": record.levelname,
                "message": record.getMessage(),
            },
            separators=(",", ":"),
        )


LOGGER = logging.getLogger("fractal_platform")


def configure_logging(*, json_output: bool = False) -> None:
    if LOGGER.handlers:
        return
    handler = logging.StreamHandler()
    handler.setFormatter(_JsonFormatter() if json_output else _PipeFormatter())
    LOGGER.addHandler(handler)
    LOGGER.setLevel(logging.INFO)
    LOGGER.propagate = False


def log_event(level: int, message: str, **fields: Any) -> None:
    """Only stable message/metadata: callers must not pass raw request/provider payloads."""
    safe_suffix = " ".join(f"{key}={value}" for key, value in sorted(fields.items()))
    LOGGER.log(level, message if not safe_suffix else f"{message} {safe_suffix}")


def worker_log(level: int, message: str, **fields: Any) -> None:
    log_event(level, message, **fields)

"""Safe worker logging. Event payloads and exception text are intentionally never logged."""

from __future__ import annotations

import logging
from typing import Any

from app.core.request_context import request_id_var


LOGGER = logging.getLogger("fractal_platform.worker")


def worker_log(level: int, message: str, **fields: Any) -> None:
    """Emit stable searchable metadata without credentials, payloads or traceback text."""
    correlation_id = request_id_var.get() or "-"
    safe_fields = " ".join(f"{key}={value}" for key, value in sorted(fields.items()))
    LOGGER.log(level, "%s | request_id=%s%s", message, correlation_id, f" | {safe_fields}" if safe_fields else "")

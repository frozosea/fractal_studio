from __future__ import annotations

from datetime import datetime
from typing import Any
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field


class PreviewInput(BaseModel):
    kind: str = Field(min_length=1, max_length=80)
    payload: dict[str, Any]


class RenderJobCreateInput(BaseModel):
    kind: str = Field(min_length=1, max_length=80)
    payload: dict[str, Any]


class RenderJobView(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: UUID
    kind: str
    status: str
    progress_percent: int
    error_code: str | None
    created_at: datetime
    finished_at: datetime | None


class DataEnvelope(BaseModel):
    data: RenderJobView


"""M2 immutable recipe and bounded-preview DTOs."""

from __future__ import annotations

from datetime import datetime
from typing import Annotated, Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field


class FractalSpec(BaseModel):
    """Version 1 canonical map input. Defaults make equivalent inputs hash identically."""

    model_config = ConfigDict(extra="forbid", populate_by_name=True, allow_inf_nan=False)

    version: Literal[1]
    seed: int | None = Field(default=None, ge=0, le=9_223_372_036_854_775_807)
    center_re: float = Field(default=0.0, alias="centerRe")
    center_im: float = Field(default=0.0, alias="centerIm")
    scale: Annotated[float, Field(gt=0.0, le=1_000_000_000.0)] = 4.0
    iterations: int = Field(default=256, ge=1, le=1_000_000)
    variant: str = Field(default="mandelbrot", min_length=1, max_length=64)
    color_map: str | None = Field(default=None, alias="colorMap", min_length=1, max_length=64)
    julia: bool = False
    julia_re: float | None = Field(default=None, alias="juliaRe")
    julia_im: float | None = Field(default=None, alias="juliaIm")
    bailout: Annotated[float, Field(gt=0.0, le=1_000_000_000.0)] = 4.0
    engine: Literal["auto", "cpu", "cuda"] = "auto"
    scalar_type: Literal["auto", "float", "double", "long_double"] = Field(
        default="auto", alias="scalarType"
    )


class RecipeInput(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    canonical_spec: FractalSpec = Field(alias="canonicalSpec")


class PreviewInput(RecipeInput):
    width: int = Field(ge=1)
    height: int = Field(ge=1)


class RecipeView(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    id: UUID
    owner_id: UUID = Field(alias="ownerId")
    canonical_spec: dict[str, object] = Field(alias="canonicalSpec")
    spec_hash: str = Field(alias="specHash")
    renderer_version: str = Field(alias="rendererVersion")
    created_at: datetime = Field(alias="createdAt")


class CursorPage(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    next_cursor: str | None = Field(default=None, alias="nextCursor")


class RecipeCollectionView(BaseModel):
    data: list[RecipeView]
    page: CursorPage

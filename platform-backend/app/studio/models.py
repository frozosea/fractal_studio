"""M2 immutable recipe and bounded-preview DTOs."""

from __future__ import annotations

from datetime import datetime
from typing import Annotated, Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, model_validator


class GradientStop(BaseModel):
    model_config = ConfigDict(extra="forbid")

    at: Annotated[float, Field(ge=0.0, le=1.0)]
    color: str = Field(pattern=r"^#[0-9A-Fa-f]{6}$")


class ColorProgram(BaseModel):
    """Compute v1 bounded custom-gradient program."""

    model_config = ConfigDict(extra="forbid", populate_by_name=True, allow_inf_nan=False)

    schema_version: Literal[1] = Field(default=1, alias="schemaVersion")
    type: Literal["gradient"] = "gradient"
    interpolation: Literal["rgb"] = "rgb"
    wrap: Literal["clamp", "repeat", "mirror"] = "clamp"
    cycles: Annotated[float, Field(gt=0.0, le=256.0)] = 1.0
    phase: float = 0.0
    interior_color: str = Field(default="#ffffff", alias="interiorColor", pattern=r"^#[0-9A-Fa-f]{6}$")
    invalid_color: str = Field(default="#ff00ff", alias="invalidColor", pattern=r"^#[0-9A-Fa-f]{6}$")
    stops: list[GradientStop] = Field(min_length=2, max_length=16)

    @model_validator(mode="after")
    def validate_stops(self) -> "ColorProgram":
        if self.stops[0].at != 0.0 or self.stops[-1].at != 1.0:
            raise ValueError("colorProgram stops must start at 0 and end at 1")
        if any(left.at >= right.at for left, right in zip(self.stops, self.stops[1:])):
            raise ValueError("colorProgram stop positions must be strictly increasing")
        return self


class ComplexParameter(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)

    re: float
    im: float


class BuiltinFormula(BaseModel):
    model_config = ConfigDict(extra="forbid")

    type: Literal["builtin"]
    id: str = Field(min_length=1, max_length=64)


class DslFormula(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)

    type: Literal["dsl"]
    source: str = Field(min_length=1, max_length=4096)
    parameters: dict[str, float | ComplexParameter] = Field(default_factory=dict, max_length=32)


FormulaDefinition = Annotated[BuiltinFormula | DslFormula, Field(discriminator="type")]


class FormulaProgram(BaseModel):
    model_config = ConfigDict(extra="forbid")

    type: Literal["formula"]
    formula: FormulaDefinition


class SequenceStep(BaseModel):
    model_config = ConfigDict(extra="forbid")

    span: int = Field(default=1, ge=1, le=1_000_000)
    program: FormulaProgram


class SequenceProgram(BaseModel):
    model_config = ConfigDict(extra="forbid")

    type: Literal["sequence"]
    repeat: Literal[True] = True
    steps: list[SequenceStep] = Field(min_length=1, max_length=64)


OrbitProgram = Annotated[FormulaProgram | SequenceProgram, Field(discriminator="type")]


class TransitionLeg(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)

    variant: str = Field(min_length=1, max_length=64)
    weight: Annotated[float, Field(gt=0.0, le=1_000_000.0)] = 1.0


class FractalSpec(BaseModel):
    """Version 1 canonical map input. Defaults make equivalent inputs hash identically."""

    model_config = ConfigDict(extra="forbid", populate_by_name=True, allow_inf_nan=False)

    version: Literal[1]
    seed: int | None = Field(default=None, ge=0, le=9_223_372_036_854_775_807)
    center_re: float = Field(default=0.0, alias="centerRe")
    center_im: float = Field(default=0.0, alias="centerIm")
    center_re_str: str | None = Field(default=None, alias="centerReStr", pattern=r"^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$", max_length=512)
    center_im_str: str | None = Field(default=None, alias="centerImStr", pattern=r"^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$", max_length=512)
    scale: Annotated[float, Field(gt=0.0, le=1_000_000_000.0)] = 4.0
    iterations: int = Field(default=256, ge=1, le=1_000_000)
    variant: str = Field(default="mandelbrot", min_length=1, max_length=64)
    color_map: str | None = Field(default=None, alias="colorMap", min_length=1, max_length=64)
    metric: Literal["escape", "min_abs", "max_abs", "envelope", "min_pairwise_dist", "mandel_ship_agree"] = "escape"
    smooth: bool = False
    color_mode: Literal["direct", "eq_full", "eq_center"] = Field(default="direct", alias="colorMode")
    cycles_per_octave: Annotated[float, Field(gt=0.0, le=64.0)] = Field(default=1.0, alias="cyclesPerOctave")
    rotation_deg: Annotated[float, Field(ge=-360.0, le=360.0)] = Field(default=0.0, alias="rotationDeg")
    pairwise_cap: int = Field(default=64, alias="pairwiseCap", ge=1, le=1_000_000)
    color_program: ColorProgram | None = Field(default=None, alias="colorProgram")
    orbit_program: OrbitProgram | None = Field(default=None, alias="orbitProgram")
    julia: bool = False
    julia_re: float | None = Field(default=None, alias="juliaRe")
    julia_im: float | None = Field(default=None, alias="juliaIm")
    bailout: Annotated[float, Field(gt=0.0, le=1_000_000_000.0)] = 4.0
    engine: Literal["auto", "cpu", "cuda", "openmp", "avx2", "avx512", "hybrid"] = "auto"
    scalar_type: Literal["auto", "float", "double", "long_double", "fp32", "fp64", "fx64", "fp80", "fp128"] = Field(
        default="auto", alias="scalarType"
    )
    transition_mode: Literal["off", "pair", "multi"] = Field(default="off", alias="transitionMode")
    transition_theta_milli_deg: int = Field(default=0, alias="transitionThetaMilliDeg", ge=-180_000, le=180_000)
    transition_from: str = Field(default="mandelbrot", alias="transitionFrom", min_length=1, max_length=64)
    transition_to: str = Field(default="burning_ship", alias="transitionTo", min_length=1, max_length=64)
    transition_legs: list[TransitionLeg] = Field(
        default_factory=lambda: [
            TransitionLeg(variant="mandelbrot", weight=1.0),
            TransitionLeg(variant="burning_ship", weight=1.0),
        ],
        alias="transitionLegs",
        min_length=1,
        max_length=4,
    )

    @model_validator(mode="after")
    def require_julia_constant(self) -> "FractalSpec":
        if self.julia and (self.julia_re is None or self.julia_im is None):
            raise ValueError("juliaRe and juliaIm are required when julia is true")
        if self.color_program is not None and self.color_map is not None:
            raise ValueError("colorMap and colorProgram are mutually exclusive")
        if self.color_program is not None and self.color_mode != "direct":
            raise ValueError("colorProgram requires colorMode=direct")
        if self.transition_mode != "off":
            if self.julia:
                raise ValueError("Julia and axis transition are separate image modes")
            if self.orbit_program is not None:
                raise ValueError("orbitProgram cannot be combined with axis transition")
            if self.metric not in {"escape", "min_abs", "max_abs", "envelope"}:
                raise ValueError("metric is not supported by axis transition")
        return self


class RecipeInput(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    canonical_spec: FractalSpec = Field(alias="canonicalSpec")


class PreviewInput(RecipeInput):
    # C++ map renderer owns the native lower bound. Keep it at the public
    # boundary so a valid Platform request never becomes a Compute 5xx.
    width: int = Field(ge=64)
    height: int = Field(ge=64)


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


class ImageOutputSpec(BaseModel):
    model_config = ConfigDict(extra="forbid")

    kind: Literal["image"]
    format: Literal["png"]
    width: int = Field(ge=64, le=4096)
    height: int = Field(ge=64, le=4096)


class VideoOutputSpec(BaseModel):
    model_config = ConfigDict(extra="forbid", populate_by_name=True)

    kind: Literal["video"]
    format: Literal["mp4"]
    width: int = Field(ge=128, le=1920)
    height: int = Field(ge=128, le=1080)
    duration_seconds: Annotated[float, Field(gt=0, le=30)] = Field(alias="durationSeconds")
    fps: int = Field(ge=1, le=60)
    # Compute zoom_video accepts 0.05..1024; keep the public range inside it.
    depth_octaves: Annotated[float, Field(ge=0.05, le=1024.0)] = Field(default=20.0, alias="depthOctaves")


class HsMeshSpec(BaseModel):
    model_config = ConfigDict(extra="forbid", populate_by_name=True, allow_inf_nan=False)

    height_scale: float | None = Field(default=None, alias="heightScale")
    height_clamp: Annotated[float | None, Field(gt=0)] = Field(default=None, alias="heightClamp")


class HsMeshOutputSpec(BaseModel):
    model_config = ConfigDict(extra="forbid", populate_by_name=True)

    kind: Literal["hs_mesh"]
    format: Literal["glb", "stl"]
    resolution: int = Field(ge=8, le=1024)
    mesh_spec: HsMeshSpec = Field(default_factory=HsMeshSpec, alias="meshSpec")


class TransitionMeshSpec(BaseModel):
    model_config = ConfigDict(extra="forbid", populate_by_name=True, allow_inf_nan=False)

    center_x: float = Field(alias="centerX")
    center_y: float = Field(alias="centerY")
    center_z: float = Field(alias="centerZ")
    extent: Annotated[float, Field(gt=0)]
    transition_from: str = Field(alias="transitionFrom", min_length=1, max_length=64)
    transition_to: str = Field(alias="transitionTo", min_length=1, max_length=64)
    bailout: Annotated[float | None, Field(gt=0)] = None
    iso: float | None = None
    engine: Literal["auto", "cpu", "cuda"] = "auto"
    scalar_type: Literal["fp32", "fp64"] = Field(default="fp32", alias="scalarType")


class TransitionMeshOutputSpec(BaseModel):
    model_config = ConfigDict(extra="forbid", populate_by_name=True)

    kind: Literal["transition_mesh"]
    format: Literal["glb", "stl"]
    resolution: int = Field(ge=8, le=1024)
    iterations: int = Field(ge=1, le=10_000)
    mesh_spec: TransitionMeshSpec = Field(alias="meshSpec")


RenderOutputSpec = Annotated[
    ImageOutputSpec | VideoOutputSpec | HsMeshOutputSpec | TransitionMeshOutputSpec,
    Field(discriminator="kind"),
]


class RenderJobCreateInput(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    recipe_id: UUID = Field(alias="recipeId")
    output: RenderOutputSpec


class RenderJobView(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    id: UUID
    recipe_id: UUID = Field(alias="recipeId")
    status: str
    progress_percent: int = Field(alias="progressPercent")
    asset_id: UUID | None = Field(default=None, alias="assetId")
    error_code: str | None = Field(default=None, alias="errorCode")
    created_at: datetime = Field(alias="createdAt")

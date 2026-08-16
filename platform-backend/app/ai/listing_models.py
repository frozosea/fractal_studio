"""Strict request and provider-output contracts for AI listing copy."""

from __future__ import annotations

from difflib import SequenceMatcher
from typing import Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

from app.marketplace.models import normalize_tags


class ListingCopyInput(BaseModel):
    """One initial generation or one revision of a saved candidate set."""

    listing_id: UUID = Field(alias="listingId")
    locale: Literal["zh", "en"]
    source_request_id: UUID | None = Field(default=None, alias="sourceRequestId")
    instruction: str | None = Field(default=None, max_length=1000)

    model_config = ConfigDict(populate_by_name=True, extra="forbid")

    @field_validator("instruction")
    @classmethod
    def normalize_instruction(cls, value: str | None) -> str | None:
        if value is None:
            return None
        normalized = value.strip()
        if not normalized:
            raise ValueError("revision instruction cannot be blank")
        return normalized

    @model_validator(mode="after")
    def valid_generation_kind(self) -> "ListingCopyInput":
        source_supplied = self.source_request_id is not None
        instruction_supplied = self.instruction is not None
        if source_supplied != instruction_supplied:
            raise ValueError("sourceRequestId and instruction must be supplied together")
        return self


class ListingCopyCandidate(BaseModel):
    """A candidate which can be copied directly into ListingUpdateInput."""

    title: str = Field(min_length=1, max_length=120)
    description: str = Field(min_length=1, max_length=4000)
    tags: list[str] = Field(min_length=1, max_length=10)

    model_config = ConfigDict(extra="forbid")

    @field_validator("title", "description")
    @classmethod
    def normalize_text(cls, value: str) -> str:
        normalized = value.strip()
        if not normalized:
            raise ValueError("listing copy cannot be blank")
        return normalized

    @field_validator("tags")
    @classmethod
    def compatible_tags(cls, value: list[str]) -> list[str]:
        # Models commonly prefix visual tags with '#'. The listing form stores
        # plain values, so remove only that presentation character before
        # applying the exact marketplace validator.
        without_hash = [tag.strip().removeprefix("#").strip() for tag in value]
        return normalize_tags(without_hash)


class ListingCopyToolResult(BaseModel):
    """The only accepted shape from SiliconFlow's forced tool call."""

    candidates: list[ListingCopyCandidate] = Field(min_length=3, max_length=3)

    model_config = ConfigDict(extra="forbid")

    @model_validator(mode="after")
    def candidates_are_distinct(self) -> "ListingCopyToolResult":
        fingerprints = {
            (
                candidate.title.casefold(),
                candidate.description.casefold(),
                tuple(candidate.tags),
            )
            for candidate in self.candidates
        }
        titles = {candidate.title.casefold() for candidate in self.candidates}
        if len(fingerprints) != 3 or len(titles) != 3:
            raise ValueError("listing copy candidates must be distinct")
        return self


class FocusedListingCopyCandidate(ListingCopyCandidate):
    """Provider-only discriminator which is removed before persistence/API output."""

    focus: Literal["composition", "color", "texture"]


class FocusedListingCopyToolResult(BaseModel):
    """Force the model to cover three distinct editorial angles."""

    candidates: list[FocusedListingCopyCandidate] = Field(min_length=3, max_length=3)

    model_config = ConfigDict(extra="forbid")

    @model_validator(mode="after")
    def candidates_cover_distinct_angles(self) -> "FocusedListingCopyToolResult":
        if [candidate.focus for candidate in self.candidates] != [
            "composition",
            "color",
            "texture",
        ]:
            raise ValueError("listing candidates must use composition, color, texture order")
        descriptions = [candidate.description.casefold() for candidate in self.candidates]
        if any(
            SequenceMatcher(None, descriptions[left], descriptions[right]).ratio() > 0.90
            for left, right in ((0, 1), (0, 2), (1, 2))
        ):
            raise ValueError("listing candidate descriptions are too similar")
        return self

    def public_candidates(self) -> list[ListingCopyCandidate]:
        return [
            ListingCopyCandidate.model_validate(candidate.model_dump(exclude={"focus"}))
            for candidate in self.candidates
        ]


def listing_candidates_json(candidates: list[ListingCopyCandidate]) -> list[dict[str, object]]:
    return [candidate.model_dump(mode="json") for candidate in candidates]

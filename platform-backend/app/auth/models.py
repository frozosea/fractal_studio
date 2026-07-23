"""M1 request, response and authenticated-principal models."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, field_validator


class CredentialsInput(BaseModel):
    email: str = Field(min_length=3, max_length=320)
    password: str = Field(min_length=12, max_length=128)

    @field_validator("email")
    @classmethod
    def valid_email(cls, value: str) -> str:
        normalized = value.strip().lower()
        if normalized.count("@") != 1 or normalized.startswith("@") or normalized.endswith("@"):
            raise ValueError("invalid email")
        return normalized


class CreatorProfileInput(BaseModel):
    handle: str = Field(min_length=3, max_length=32, pattern=r"^[a-z0-9_]+$")
    display_name: str = Field(min_length=1, max_length=120, alias="displayName")

    model_config = ConfigDict(populate_by_name=True)

    @field_validator("handle")
    @classmethod
    def lower_handle(cls, value: str) -> str:
        if value != value.lower():
            raise ValueError("handle must be lowercase")
        return value


class CreatorProfileView(BaseModel):
    handle: str
    display_name: str = Field(alias="displayName")

    model_config = ConfigDict(populate_by_name=True)


class UserView(BaseModel):
    id: UUID
    email: str
    status: Literal["active", "disabled"]
    roles: list[Literal["creator", "finance_operator"]]
    creator_profile: CreatorProfileView | None = Field(default=None, alias="creatorProfile")

    model_config = ConfigDict(populate_by_name=True)


class CsrfTokenView(BaseModel):
    token: str


@dataclass(frozen=True, slots=True)
class AccessPrincipal:
    user_id: UUID
    session_id: UUID
    roles: frozenset[str]
    session_token: str

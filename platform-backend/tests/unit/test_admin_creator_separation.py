from uuid import UUID

from fastapi import HTTPException, Request
import pytest

from app.auth.models import AccessPrincipal, CreatorProfileInput
from app.auth.service import upsert_creator_profile


@pytest.mark.asyncio
async def test_administrator_cannot_create_creator_profile() -> None:
    principal = AccessPrincipal(
        user_id=UUID("00000000-0000-4000-8000-000000000004"),
        session_id=UUID("00000000-0000-4000-8000-000000000005"),
        roles=frozenset({"admin"}),
        session_token="test-session-token",
    )
    request = Request(
        {"type": "http", "method": "PATCH", "path": "/v1/me/creator-profile", "headers": []}
    )

    with pytest.raises(HTTPException) as error:
        await upsert_creator_profile(
            principal,
            CreatorProfileInput(handle="administrator", displayName="Administrator"),
            "admin-profile-attempt",
            request,
        )

    assert error.value.status_code == 409
    assert error.value.detail == "admin_creator_role_conflict"

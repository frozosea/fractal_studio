from datetime import UTC, datetime
from uuid import UUID

from fastapi import HTTPException
from pydantic import ValidationError
import pytest

from app.admin.models import AdminListingModerationInput, AdminUserUpdateInput
from app.admin.router import _decode_cursor, _encode_cursor
from app.auth.models import UserView


def test_user_view_accepts_admin_role() -> None:
    user = UserView(
        id="00000000-0000-4000-8000-000000000001",
        email="admin@example.test",
        status="active",
        roles=["admin"],
    )
    assert user.roles == ["admin"]


def test_admin_user_update_requires_a_change() -> None:
    with pytest.raises(ValidationError):
        AdminUserUpdateInput()


def test_admin_user_update_rejects_duplicate_roles() -> None:
    with pytest.raises(ValidationError):
        AdminUserUpdateInput(privilegedRoles=["admin", "admin"])


def test_moderation_reason_is_trimmed() -> None:
    value = AdminListingModerationInput(action="unpublish", reason="  policy violation  ")
    assert value.reason == "policy violation"


def test_moderation_reason_cannot_be_blank() -> None:
    with pytest.raises(ValidationError):
        AdminListingModerationInput(action="archive", reason="   ")


def test_admin_cursor_is_bound_to_filters() -> None:
    created_at = datetime(2026, 7, 30, tzinfo=UTC)
    item_id = UUID("00000000-0000-4000-8000-000000000002")
    filters = {"q": "artist", "status": "published"}
    cursor = _encode_cursor(
        kind="admin_listings", filters=filters, created_at=created_at, item_id=item_id
    )
    assert _decode_cursor(cursor, kind="admin_listings", filters=filters) == (
        created_at,
        item_id,
    )
    with pytest.raises(HTTPException) as error:
        _decode_cursor(
            cursor,
            kind="admin_listings",
            filters={"q": "different", "status": "published"},
        )
    assert error.value.status_code == 422

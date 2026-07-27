from pydantic import ValidationError
import pytest

from app.auth.models import CredentialsInput, CreatorProfileInput


@pytest.mark.parametrize("password", ["short", "x" * 129])
def test_credentials_reject_invalid_password_length(password: str) -> None:
    with pytest.raises(ValidationError):
        CredentialsInput(email="user@example.test", password=password)


@pytest.mark.parametrize("handle", ["UPPER", "no-dash", "ab", "x" * 33])
def test_creator_profile_rejects_invalid_handle(handle: str) -> None:
    with pytest.raises(ValidationError):
        CreatorProfileInput(handle=handle, displayName="Creator")


def test_creator_profile_serializes_camel_case() -> None:
    profile = CreatorProfileInput(handle="creator_01", displayName="Creator")
    assert profile.model_dump(by_alias=True) == {"handle": "creator_01", "displayName": "Creator"}

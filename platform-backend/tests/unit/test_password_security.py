import base64
import hashlib

from app.auth.service import _hash_password, _password_needs_rehash, _verify_password


def _legacy_hash(password: str) -> str:
    salt = bytes(range(16))
    derived = hashlib.scrypt(password.encode(), salt=salt, n=2**14, r=8, p=1)
    return "scrypt$16384$8$1${}${}".format(
        base64.urlsafe_b64encode(salt).decode(),
        base64.urlsafe_b64encode(derived).decode(),
    )


def test_new_password_hash_uses_hardened_scrypt_parameters() -> None:
    password = "correct-horse-01"
    encoded = _hash_password(password, salt=bytes(range(16)))

    assert encoded.startswith("scrypt$131072$8$1$")
    assert password not in encoded
    assert _verify_password(password, encoded)
    assert not _verify_password("wrong-password", encoded)
    assert not _password_needs_rehash(encoded)


def test_legacy_password_hash_remains_valid_but_requires_rehash() -> None:
    encoded = _legacy_hash("correct-horse-01")

    assert _verify_password("correct-horse-01", encoded)
    assert _password_needs_rehash(encoded)


def test_malformed_password_hash_fails_closed() -> None:
    assert not _verify_password("correct-horse-01", "not-a-password-hash")
    assert _password_needs_rehash("not-a-password-hash")

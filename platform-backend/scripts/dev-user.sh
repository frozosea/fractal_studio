#!/usr/bin/env bash
set -euo pipefail

# Creates or logs in a local dev user through real M1 endpoints. It stores only
# local credentials/cookie beside this script; both files are gitignored.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CREDENTIALS_FILE="${DEV_USER_CREDENTIALS_FILE:-$ROOT_DIR/scripts/.dev-user.env}"
COOKIE_FILE="${DEV_USER_COOKIE_FILE:-$ROOT_DIR/scripts/.dev-user.cookies}"
API_URL="${API_URL:-http://localhost:18000}"

if [[ -f "$CREDENTIALS_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$CREDENTIALS_FILE"
fi

EMAIL="${DEV_USER_EMAIL:-dev-user@example.test}"
PASSWORD="${DEV_USER_PASSWORD:-}"
if [[ -z "$PASSWORD" ]]; then
  PASSWORD="$(python3 -c 'import secrets; print(secrets.token_urlsafe(24))')"
fi

payload="$({ DEV_USER_EMAIL="$EMAIL" DEV_USER_PASSWORD="$PASSWORD" python3 - <<'PY'
import json
import os

print(json.dumps({"email": os.environ["DEV_USER_EMAIL"], "password": os.environ["DEV_USER_PASSWORD"]}))
PY
})"

umask 077
response_file="$(mktemp)"
trap 'rm -f "$response_file"' EXIT

request() {
  local route="$1"
  curl --noproxy '*' -sS \
    -o "$response_file" \
    -w '%{http_code}' \
    -c "$COOKIE_FILE" \
    -b "$COOKIE_FILE" \
    -H 'Content-Type: application/json' \
    -X POST "$API_URL$route" \
    --data "$payload"
}

status_code="$(request /v1/auth/register)"
if [[ "$status_code" == "201" ]]; then
  action="registered"
elif [[ "$status_code" == "409" ]]; then
  status_code="$(request /v1/auth/login)"
  if [[ "$status_code" != "200" ]]; then
    cat "$response_file" >&2
    exit 1
  fi
  action="logged in"
else
  cat "$response_file" >&2
  exit 1
fi

printf 'DEV_USER_EMAIL=%q\nDEV_USER_PASSWORD=%q\nAPI_URL=%q\n' \
  "$EMAIL" "$PASSWORD" "$API_URL" > "$CREDENTIALS_FILE"

printf 'Dev user %s: %s\n' "$action" "$EMAIL"
printf 'Credentials: %s\nCookie jar: %s\n' "$CREDENTIALS_FILE" "$COOKIE_FILE"
printf 'Authenticated check:\n'
curl --noproxy '*' -fsS -b "$COOKIE_FILE" "$API_URL/v1/me"
printf '\n'

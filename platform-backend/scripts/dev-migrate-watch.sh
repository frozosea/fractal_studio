#!/bin/sh
set -eu

uv run --no-sync alembic upgrade head
touch /tmp/migrations-ready

exec uv run --no-sync watchfiles \
  --filter python \
  "uv run --no-sync alembic upgrade head" \
  migrations

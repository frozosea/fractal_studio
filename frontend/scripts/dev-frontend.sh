#!/bin/sh
set -eu

expected_lock="$(sha256sum pnpm-lock.yaml | awk '{print $1}')"
lock_marker="node_modules/.fractal-lock.sha256"

if [ ! -f "$lock_marker" ] || [ "$(cat "$lock_marker")" != "$expected_lock" ]; then
  echo "Refreshing frontend dependencies for updated pnpm-lock.yaml"
  CI=true pnpm install --frozen-lockfile
  printf '%s\n' "$expected_lock" > "$lock_marker"
fi

exec pnpm dev --hostname 0.0.0.0 --port 3000

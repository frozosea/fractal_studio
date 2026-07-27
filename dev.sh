#!/usr/bin/env bash
set -euo pipefail

# One local product stand: Next browser -> Platform -> private Compute.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

if [[ "${1:-}" == "--down" ]]; then
  docker compose -f docker-compose.dev.yml down
  exit 0
fi

exec docker compose -f docker-compose.dev.yml up --build --remove-orphans "$@"

#!/usr/bin/env bash
set -euo pipefail

export LC_ALL=C

if ! PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"; then
  echo "Cannot locate the Fractal Studio project root." >&2
  exit 2
fi
readonly PROJECT_ROOT
readonly LEGACY_ROOT="${FRACTAL_LEGACY_ROOT:-$(dirname "$PROJECT_ROOT")}"
readonly -a LEGACY_DIRS=("C_mandelbrot" "cfiles" "cuda_mandelbrot" "Mandelbrot_set")

if [[ -n "${FRACTAL_LEGACY_BASELINE:-}" ]]; then
  BASELINE_FILE="$FRACTAL_LEGACY_BASELINE"
else
  if ! GIT_DIR="$(git -C "$PROJECT_ROOT" rev-parse --absolute-git-dir 2>/dev/null)"; then
    echo "Cannot locate the Git directory for $PROJECT_ROOT." >&2
    echo "Set FRACTAL_LEGACY_BASELINE to an explicit baseline path." >&2
    exit 2
  fi
  BASELINE_FILE="$GIT_DIR/fractal-studio/legacy-immutable.sha256"
fi
readonly BASELINE_FILE

TEMP_DIR=""
BASELINE_TEMP=""

cleanup() {
  if [[ -n "$TEMP_DIR" ]]; then
    rm -rf -- "$TEMP_DIR" || true
  fi
  if [[ -n "$BASELINE_TEMP" ]]; then
    rm -f -- "$BASELINE_TEMP" || true
  fi
}
trap cleanup EXIT

usage() {
  cat <<'EOF'
Usage: scripts/check_legacy_immutable.sh [--init | --update]

Without an option, compare the legacy source trees with the trusted local
baseline.  Use --init once to trust their current state, or --update to accept
an intentional change.  The baseline is stored below .git by default.

Environment overrides (primarily useful for isolated checks):
  FRACTAL_LEGACY_ROOT       Parent directory of the legacy source trees
  FRACTAL_LEGACY_BASELINE   Explicit path for the integrity baseline
EOF
}

die() {
  echo "$*" >&2
  exit 1
}

ensure_temp_dir() {
  if [[ -z "$TEMP_DIR" ]]; then
    local created
    if ! created="$(mktemp -d "${TMPDIR:-/tmp}/fractal-legacy.XXXXXXXX")"; then
      echo "Failed to create a temporary directory for the integrity check." >&2
      return 1
    fi
    if [[ -z "$created" || ! -d "$created" ]]; then
      echo "Temporary directory creation returned an invalid path." >&2
      return 1
    fi
    TEMP_DIR="$created"
  fi
}

hash_file() {
  local path="$1"
  local line digest

  # Hash through stdin so GNU sha256sum never escapes the digest line for a
  # filename containing a backslash or newline.
  if ! line="$(sha256sum <"$path")"; then
    echo "Failed to hash integrity input: $path" >&2
    return 1
  fi
  digest="${line%% *}"
  if [[ ! "$digest" =~ ^[0-9a-f]{64}$ ]]; then
    echo "Invalid SHA-256 output for integrity input: $path" >&2
    return 1
  fi
  printf '%s\n' "$digest"
}

present_directory_count() {
  local count=0
  local name
  for name in "${LEGACY_DIRS[@]}"; do
    if [[ -e "$LEGACY_ROOT/$name" || -L "$LEGACY_ROOT/$name" ]]; then
      ((count += 1))
    fi
  done
  printf '%s\n' "$count"
}

# Build a canonical byte stream instead of hashing find(1)'s absolute-path
# output.  This makes the fingerprint independent of the checkout location and
# covers names, entry types, permission bits, regular-file contents, and link
# targets.  Special files are rejected rather than silently omitted.
hash_tree() {
  local tree="$1"
  local entries records link_target
  local path relative mode digest

  if ! ensure_temp_dir; then
    return 1
  fi
  entries="$TEMP_DIR/entries"
  records="$TEMP_DIR/records"
  link_target="$TEMP_DIR/link-target"

  if ! find "$tree" -print0 >"$entries"; then
    echo "Failed to enumerate legacy tree: $tree" >&2
    return 1
  fi
  if ! sort -z -o "$entries" "$entries"; then
    echo "Failed to sort legacy tree entries: $tree" >&2
    return 1
  fi
  if ! : >"$records"; then
    echo "Failed to create integrity records for: $tree" >&2
    return 1
  fi

  while IFS= read -r -d '' path; do
    relative="${path#"${LEGACY_ROOT%/}/"}"
    if ! mode="$(stat -c '%a' -- "$path")"; then
      echo "Failed to inspect legacy entry: $relative" >&2
      return 1
    fi

    if [[ -L "$path" ]]; then
      if ! readlink -z -- "$path" >"$link_target"; then
        echo "Failed to read legacy symlink: $relative" >&2
        return 1
      fi
      if ! digest="$(hash_file "$link_target")"; then
        echo "Failed to hash legacy symlink target: $relative" >&2
        return 1
      fi
      if ! printf 'L\0%s\0%s\0%s\0' "$relative" "$mode" "$digest" >>"$records"; then
        echo "Failed to record legacy symlink: $relative" >&2
        return 1
      fi
    elif [[ -d "$path" ]]; then
      if ! printf 'D\0%s\0%s\0' "$relative" "$mode" >>"$records"; then
        echo "Failed to record legacy directory: $relative" >&2
        return 1
      fi
    elif [[ -f "$path" ]]; then
      if ! digest="$(hash_file "$path")"; then
        echo "Failed to hash legacy file: $relative" >&2
        return 1
      fi
      if ! printf 'F\0%s\0%s\0%s\0' "$relative" "$mode" "$digest" >>"$records"; then
        echo "Failed to record legacy file: $relative" >&2
        return 1
      fi
    else
      echo "Unsupported special file in legacy tree: $relative" >&2
      return 1
    fi
  done <"$entries"

  if ! digest="$(hash_file "$records")"; then
    echo "Failed to hash integrity records for: $tree" >&2
    return 1
  fi
  printf '%s\n' "$digest"
}

write_manifest() {
  local output="$1"
  local name path digest

  if ! ensure_temp_dir; then
    return 1
  fi
  if ! printf '# fractal-studio legacy integrity baseline v1\n' >"$output"; then
    echo "Failed to create legacy integrity manifest: $output" >&2
    return 1
  fi
  for name in "${LEGACY_DIRS[@]}"; do
    path="$LEGACY_ROOT/$name"
    if [[ ! -e "$path" && ! -L "$path" ]]; then
      if ! printf '%s\tmissing\n' "$name" >>"$output"; then
        echo "Failed to record missing legacy directory: $name" >&2
        return 1
      fi
      continue
    fi
    if [[ -L "$path" || ! -d "$path" ]]; then
      echo "Expected a real directory at $path" >&2
      return 1
    fi
    if ! digest="$(hash_tree "$path")"; then
      return 1
    fi
    if [[ ! "$digest" =~ ^[0-9a-f]{64}$ ]]; then
      echo "Invalid tree digest for legacy directory: $name" >&2
      return 1
    fi
    if ! printf '%s\tsha256:%s\n' "$name" "$digest" >>"$output"; then
      echo "Failed to record legacy directory digest: $name" >&2
      return 1
    fi
  done
}

store_baseline() {
  local action="$1"
  local baseline_dir

  baseline_dir="$(dirname "$BASELINE_FILE")"
  if ! mkdir -p -- "$baseline_dir"; then
    die "Could not create the baseline directory: $baseline_dir"
  fi
  if ! BASELINE_TEMP="$(mktemp "$baseline_dir/.legacy-immutable.XXXXXXXX")"; then
    BASELINE_TEMP=""
    die "Could not create a temporary baseline below: $baseline_dir"
  fi
  if [[ -z "$BASELINE_TEMP" || ! -f "$BASELINE_TEMP" ]]; then
    die "Temporary baseline creation returned an invalid path."
  fi
  if ! write_manifest "$BASELINE_TEMP"; then
    die "Could not create the legacy integrity baseline."
  fi
  if ! chmod 0644 "$BASELINE_TEMP"; then
    die "Could not set permissions on the temporary integrity baseline."
  fi
  if ! mv -f -- "$BASELINE_TEMP" "$BASELINE_FILE"; then
    die "Could not install the integrity baseline: $BASELINE_FILE"
  fi
  BASELINE_TEMP=""
  echo "Legacy integrity baseline $action: $BASELINE_FILE"
}

MODE="verify"
if (($# > 1)); then
  usage >&2
  exit 2
fi
case "${1:-}" in
  "") ;;
  --init) MODE="init" ;;
  --update) MODE="update" ;;
  -h|--help)
    usage
    exit 0
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac

PRESENT_COUNT="$(present_directory_count)"

case "$MODE" in
  init)
    [[ ! -e "$BASELINE_FILE" ]] || die \
      "Baseline already exists; use --update to replace it: $BASELINE_FILE"
    ((PRESENT_COUNT > 0)) || die \
      "Cannot initialize a baseline: no legacy directories exist under $LEGACY_ROOT"
    store_baseline "initialized"
    exit 0
    ;;
  update)
    [[ -f "$BASELINE_FILE" ]] || die \
      "No baseline exists; use --init first: $BASELINE_FILE"
    ((PRESENT_COUNT > 0)) || die \
      "Refusing to update the baseline with every legacy directory missing."
    store_baseline "updated"
    exit 0
    ;;
esac

if [[ ! -e "$BASELINE_FILE" ]]; then
  if ((PRESENT_COUNT == 0)); then
    echo "Legacy source trees are not present under $LEGACY_ROOT; integrity check skipped."
    exit 0
  fi
  echo "Legacy source trees exist, but no trusted baseline is available." >&2
  echo "Inspect them, then run scripts/check_legacy_immutable.sh --init once." >&2
  echo "Expected baseline: $BASELINE_FILE" >&2
  exit 1
fi
[[ -f "$BASELINE_FILE" ]] || die "Baseline is not a regular file: $BASELINE_FILE"

if ! ensure_temp_dir; then
  die "Could not prepare temporary storage for the integrity check."
fi
CURRENT_MANIFEST="$TEMP_DIR/current.sha256"
if ! write_manifest "$CURRENT_MANIFEST"; then
  die "Could not calculate the current legacy integrity manifest."
fi

if cmp -s -- "$BASELINE_FILE" "$CURRENT_MANIFEST"; then
  echo "Legacy source trees match the trusted baseline."
  exit 0
fi

echo "Legacy source tree integrity check failed." >&2
diff -u --label trusted-baseline --label current-state \
  "$BASELINE_FILE" "$CURRENT_MANIFEST" >&2 || true
echo "If this change is intentional, inspect it before running:" >&2
echo "  scripts/check_legacy_immutable.sh --update" >&2
exit 1

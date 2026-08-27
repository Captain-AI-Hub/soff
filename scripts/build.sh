#!/usr/bin/env sh
set -eu

MODE="release"
IDA_PLUGIN=0
IDA_SDK="ida-sdk-94-main/src"
SKIP_SOFF=0
SKIP_DESKTOP=0
DESKTOP_BUNDLES=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --mode)
      MODE="$2"
      shift 2
      ;;
    --ida-plugin)
      IDA_PLUGIN=1
      shift
      ;;
    --ida-sdk)
      IDA_SDK="$2"
      shift 2
      ;;
    --skip-soff)
      SKIP_SOFF=1
      shift
      ;;
    --skip-desktop)
      SKIP_DESKTOP=1
      shift
      ;;
    --desktop-bundles)
      DESKTOP_BUNDLES="$2"
      shift 2
      ;;
    -h|--help)
      cat <<'EOF'
Usage: scripts/build.sh [options]

Options:
  --mode release|debug    Build mode, default: release
  --ida-plugin           Also build soff_ida
  --ida-sdk PATH         IDA SDK src path, default: ida-sdk-94-main/src
  --skip-soff            Skip xmake build
  --skip-desktop         Skip desktop build
  --desktop-bundles LIST Build specific Tauri bundles, e.g. deb, nsis, dmg
EOF
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 2
      ;;
  esac
done

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
DESKTOP_DIR="$REPO_ROOT/desktop"
RESOURCES_DIR="$DESKTOP_DIR/src-tauri/resources"

run() {
  echo "==> $*"
  "$@"
}

ffi_name() {
  case "$(uname -s)" in
    Darwin) echo "libsoff_ffi.dylib" ;;
    Linux) echo "libsoff_ffi.so" ;;
    MINGW*|MSYS*|CYGWIN*) echo "soff_ffi.dll" ;;
    *) echo "libsoff_ffi.so" ;;
  esac
}

copy_soff_ffi_to_desktop() {
  name=$(ffi_name)
  if [ ! -d "$REPO_ROOT/build" ]; then
    echo "build/ does not exist; run the Soff build first." >&2
    exit 1
  fi
  ffi=$(find "$REPO_ROOT/build" -type f -name "$name" | sort | tail -n 1)
  if [ -z "$ffi" ]; then
    echo "$name was not found under build/" >&2
    exit 1
  fi
  mkdir -p "$RESOURCES_DIR"
  cp "$ffi" "$RESOURCES_DIR/$name"
  echo "Copied $ffi -> $RESOURCES_DIR"
}

if [ "$SKIP_SOFF" -eq 0 ]; then
  cd "$REPO_ROOT"
  if [ "$IDA_PLUGIN" -eq 1 ]; then
    run xmake config -y -m "$MODE" --ida_plugin=y "--ida_sdk=$IDA_SDK"
    run xmake require -y
    for target in soff_cli soff_smoke soff_ffi soff_ida; do
      run xmake build -y "$target"
    done
  else
    run xmake config -y -m "$MODE" --ida_plugin=n
    run xmake require -y
    for target in soff_cli soff_smoke soff_ffi; do
      run xmake build -y "$target"
    done
  fi
  copy_soff_ffi_to_desktop
fi

if [ "$SKIP_DESKTOP" -eq 0 ]; then
  cd "$DESKTOP_DIR"
  if [ ! -d node_modules ]; then
    if [ "${CI:-}" = "true" ]; then
      run bun install --frozen-lockfile
    else
      run bun install
    fi
  fi
  if [ -n "$DESKTOP_BUNDLES" ]; then
    run bun run tauri build --bundles "$DESKTOP_BUNDLES"
  else
    run bun run tauri build
  fi
fi

echo "Build complete."

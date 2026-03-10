#!/usr/bin/env bash

set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

build_native() {
  echo "==== Building native library (CMake/NDK) ===="
  (
    cd "$ROOT_DIR/native"
    ./build_native.sh
  )
}

build_monkeyq() {
  echo "==== Building monkeyq.jar (Gradle) ===="
  (
    cd "$ROOT_DIR"
    ./build_monkeyq.sh
  )
}

case "${1:-all}" in
  native)
    build_native
    ;;
  monkeyq)
    build_monkeyq
    ;;
  all)
    build_native
    build_monkeyq
    ;;
  *)
    echo "Usage: $0 [native|monkeyq|all]"
    exit 1
    ;;
esac

echo "Done."


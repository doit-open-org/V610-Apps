#!/bin/sh
# Host tests for the parts of the sample that are plain C and need no board.
set -eu

SAMPLE_DIR="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
RUNTIME_DIR="${MIPK_PATH:-${APP_SDK_PATH:?source the SDK export.sh first}/components/mipk_runtime/mipk}"
BUILD_DIR="${TMPDIR:-/tmp}/mipk_yolo_pose_tests"
mkdir -p "$BUILD_DIR"

cc -std=c11 -O2 -Wall -Wextra -Werror \
    -I"$SAMPLE_DIR/main/src" \
    -I"$RUNTIME_DIR/include" \
    "$SAMPLE_DIR/tests/test_keypoints.c" \
    "$SAMPLE_DIR/main/src/keypoints.c" \
    -o "$BUILD_DIR/test_keypoints"
"$BUILD_DIR/test_keypoints" "$SAMPLE_DIR/keypoints"

echo "mipk_keypoints host tests passed"

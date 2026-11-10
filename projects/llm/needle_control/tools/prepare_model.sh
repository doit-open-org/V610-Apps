#!/bin/sh
# Downloads needle3.cact from Hugging Face and slices it to 8 layers:
#
#   sh tools/prepare_model.sh [output directory]
#
# Writes needle3-l8.cact, by default into out/. Needs curl or wget, Python 3
# and numpy.
set -eu

APP_DIR=$(cd "$(dirname "$0")/.." && pwd)
OUT_DIR=${1:-$APP_DIR/out}
SLICER=$APP_DIR/components/needle_engine/tools/slice_cact.py
URL=https://huggingface.co/Cactus-Compute/needle3/resolve/main/needle3.cact

mkdir -p "$OUT_DIR"
if [ ! -f "$OUT_DIR/needle3.cact" ]; then
    echo "downloading $URL"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o "$OUT_DIR/needle3.cact.part" "$URL"
    else
        wget -O "$OUT_DIR/needle3.cact.part" "$URL"
    fi
    mv "$OUT_DIR/needle3.cact.part" "$OUT_DIR/needle3.cact"
fi
python3 "$SLICER" --layers 8 --in "$OUT_DIR/needle3.cact" --out "$OUT_DIR/needle3-l8.cact"

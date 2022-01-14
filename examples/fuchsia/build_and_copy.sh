#!/bin/bash
# set -e # Exit if any program returns an error.

rm -r $ENGINE_DIR/out/fuchsia_debug_x64/hello_fuchsia_embedder_far/data/flutter_assets
$ENGINE_DIR/flutter/tools/gn --fuchsia --no-lto --no-goma --embedder-for-target --no-prebuilt-dart-sdk
ninja -C $ENGINE_DIR/out/fuchsia_debug_x64 $@
cp $ENGINE_DIR/out/fuchsia_debug_x64/hello_fuchsia_embedder-0.far $FUCHSIA_DIR/prebuilt/

#!/bin/bash

rm -r $ENGINE_DIR/out/fuchsia_debug_unopt_x64/hello_fuchsia_embedder_far/data/flutter_assets

set -e # Exit if any program returns an error.

$ENGINE_DIR/flutter/tools/gn --fuchsia --unoptimized --no-lto --no-goma --embedder-for-target --no-prebuilt-dart-sdk
ninja -C $ENGINE_DIR/out/fuchsia_debug_unopt_x64 $@

cd $FUCHSIA_DIR
fx pm publish -a -f $ENGINE_DIR/out/fuchsia_debug_unopt_x64/hello_fuchsia_embedder-0.far -repo $(fx get-build-dir)/amber-files
ffx session restart
sleep 15
ffx session add "fuchsia-pkg://fuchsia.com/hello_fuchsia_embedder#meta/component.cm"
# ELEMENT_NAME=$(ffx component list | grep elements | tr ":" "\n" | tail -n 1)
# ffx component start "/core/session-manager/session:session/element_manager/elements:$ELEMENT_NAME"
# ffx component destroy "/core/session-manager/session:session/element_manager/elements:embedder"
# ffx component create "/core/session-manager/session:session/element_manager/elements:embedder" "fuchsia-pkg://fuchsia.com/hello_fuchsia_embedder#meta/component.cm"
# ffx component start "/core/session-manager/session:session/element_manager/elements:embedder"
fx log --suppress cobalt,wifi,wlan,netstack,audio_core --pretty

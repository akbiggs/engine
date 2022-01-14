#!/bin/bash
# set -e # Exit if any program returns an error.

cd $ENGINE_DIR/flutter/examples/glfw/myapp
flutter build bundle
cp -r build/flutter_assets ../../fuchsia/
cd -

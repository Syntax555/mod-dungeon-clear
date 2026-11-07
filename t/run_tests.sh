#!/bin/bash
set -e

# Find repo root relative to the script location
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
REPO_ROOT="$( cd "${SCRIPT_DIR}/../../.." && pwd )"
BUILD_DIR="${REPO_ROOT}/build"

echo "=========================================================="
echo " Dungeon Clear Test Runner"
echo "=========================================================="
echo "Repository Root: ${REPO_ROOT}"
echo "Build Directory: ${BUILD_DIR}"
echo "----------------------------------------------------------"

# Configure with unit testing enabled; further arguments go to the configure
# step, e.g. -DCMAKE_BUILD_TYPE=Release.
echo "Configuring build system with BUILD_TESTING=ON..."
cd "${REPO_ROOT}"
cmake -B build -DBUILD_TESTING=ON "$@"

# Compile the dungeon_clear_tests target specifically
echo "Building dungeon_clear_tests target..."
cmake --build build --target dungeon_clear_tests -j$(nproc)

# Run the whole suite: the binary holds only this module's tests. A name filter
# silently skipped every suite it did not list. The navmesh route probes skip
# themselves unless DC_PROBE_MMAPS names a dir with mmaps/ for their map.
echo "Running unit tests..."
cd "${BUILD_DIR}"
./dungeon_clear_tests

echo "----------------------------------------------------------"
echo "Tests completed successfully!"
echo "=========================================================="

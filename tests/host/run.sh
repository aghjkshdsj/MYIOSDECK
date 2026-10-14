#!/bin/bash
# Host tests: the app's pure Swift logic compiled for the CI host (Linux or macOS) with the test
# programs in tests/host, no simulator and no device. Each test program exits nonzero on a failure.
#   tests/host/run.sh [<build-dir>]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:-${RUNNER_TEMP:-/tmp}/host-tests}"
mkdir -p "$OUT"
swiftc --version | head -1
# Steam Cloud saves: path mapping, comparison, sync decisions, backup retention.
swiftc -O -o "$OUT/steam-cloud-tests" "$ROOT/App/Sources/Steam/SteamCloudCore.swift" "$HERE/steam_cloud/main.swift"
"$OUT/steam-cloud-tests" | tee "$OUT/steam-cloud-tests.txt"

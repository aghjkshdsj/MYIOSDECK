#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Comparison method adapted from ../fxi/ci-test.sh; existing harness untouched.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$HERE/measure.py" "$@"

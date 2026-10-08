#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The newest successful run of a stage workflow for this build's branch, else main's.
#   tools/latest-run.sh <workflow file>    (GH_TOKEN, GITHUB_REPOSITORY, GITHUB_REF_NAME set)
# Branch first, so a side branch that rebuilds a stage (another Madeira pin) never feeds
# main's IPA, and main never picks up a side branch's libraries.
set -euo pipefail
wf="${1:?workflow file}"
for br in "${GITHUB_HEAD_REF:-${GITHUB_REF_NAME:-main}}" main; do
    run=$(gh run list -R "$GITHUB_REPOSITORY" --workflow "$wf" --branch "$br" --status success --limit 1 \
        --json databaseId --jq '.[0].databaseId // empty' 2>/dev/null || true)
    if [ -n "$run" ]; then
        echo "$wf: run $run ($br)" >&2
        echo "$run"
        exit 0
    fi
done
echo "$wf: no successful run on ${GITHUB_HEAD_REF:-${GITHUB_REF_NAME:-main}} or main" >&2

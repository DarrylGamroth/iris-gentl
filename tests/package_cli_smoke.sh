#!/usr/bin/env bash
set -euo pipefail

cli=$1
cti=$2
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

IRIS_FAULT=example "$cli" describe --cti "$cti" Width --json | grep -F '"increment":16'

IRIS_FAULT=progressive-controls "$cli" snapshot --cti "$cti" \
    --set TriggerSelector=FrameStart TriggerMode > "$tmp/before"
IRIS_FAULT=progressive-controls "$cli" snapshot --cti "$cti" \
    --set TriggerSelector=ExposureStart TriggerMode > "$tmp/after"
diff_status=0
"$cli" diff "$tmp/before" "$tmp/after" --json > "$tmp/diff" || diff_status=$?
test "$diff_status" -eq 1
grep -E '"selectors":\{"TriggerSelector":"ExposureStart"\},"change":"added"' "$tmp/diff"
grep -E '"selectors":\{"TriggerSelector":"FrameStart"\},"change":"removed"' "$tmp/diff"

IRIS_FAULT=events-snapshot "$cli" event 2 --cti "$cti" --json \
    | grep -E '"device_timestamp":9007199254740993.*"name":"EventCounter","type":"integer","value":"41"'

IRIS_FAULT=progressive-complete "$cli" grab 2 --cti "$cti" --stream-only --buffers 1 \
    --output "$tmp/frames.raw" | grep -F 'frame=2 width=2 height=4'
test "$(wc -c < "$tmp/frames.raw")" -eq 32

IRIS_FAULT=progressive-complete "$cli" play 3 --cti "$cti" --stream-only --buffers 1 --json \
    | grep -E '"complete":3,"incomplete":0,"unsupported_payload":0,"invalid_image":0,"timeouts":0,.*"interval_samples":2'

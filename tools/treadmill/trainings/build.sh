#!/bin/sh
# Builds every training spec in this directory into ../workouts/<id>.fit with treadmill-fit,
# or with --check verifies that the committed .fit files are exactly what the specs build.
# TREADMILL_FIT selects the binary (default: treadmill-fit on PATH).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tool=${TREADMILL_FIT:-treadmill-fit}
mode=build
[ "${1:-}" = "--check" ] && mode=check
status=0
for spec in "$here"/*.json; do
    id=$(basename "$spec" .json)
    fit="$here/../workouts/$id.fit"
    if [ "$mode" = check ]; then
        "$tool" check "$spec" "$fit" || status=1
    else
        "$tool" build "$spec" "$fit" --overwrite || status=1
    fi
done
if [ "$mode" = check ]; then
    for fit in "$here"/../workouts/*.fit; do
        [ -e "$fit" ] || continue
        [ -e "$here/$(basename "$fit" .fit).json" ] || { echo "no spec for $fit" >&2; status=1; }
    done
fi
exit $status

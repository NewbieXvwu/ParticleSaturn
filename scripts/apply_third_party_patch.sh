#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec cmake -DPARTICLESATURN_PATCH="${1:-}" -P "$repo_root/scripts/apply_third_party_patch.cmake"

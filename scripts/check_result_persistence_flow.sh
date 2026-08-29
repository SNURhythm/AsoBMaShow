#!/bin/sh
set -eu

repository_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
exec python3 "$repository_root/scripts/check_result_persistence_flow.py" "$repository_root"

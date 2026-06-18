#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

rg -q "APP_LOG_FORMAT_VERSION" "$repo_root/src/main.c"
rg -q "\"firmware:" "$repo_root/src/main.c"
rg -q "gatt_diag=1" "$repo_root/src/main.c"

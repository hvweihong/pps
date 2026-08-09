#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if grep -q '^CONFIG_RADIO_BRIDGE_VALIDATION_CDC=y$' "${ROOT}/prj.conf"; then
	echo "CDC validation must be disabled in the production config" >&2
	exit 1
fi
grep -q '^CONFIG_RADIO_BRIDGE_VALIDATION_CDC=y$' "${ROOT}/validation.conf"
echo "CDC validation config split: PASS"

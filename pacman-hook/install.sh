#!/usr/bin/env bash
# Register persist hooks + payload + modprobe.d vrm=1.
# Usage: sudo bash install.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"

if [[ "$(id -u)" -ne 0 ]]; then
	echo "Need root: sudo bash $0" >&2
	exit 1
fi

exec /usr/bin/python3 "$REPO/nct6687_vrm_persist.py" install

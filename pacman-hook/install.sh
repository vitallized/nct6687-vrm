#!/usr/bin/env bash
# Install pacman hook + stable inject copy + modprobe.d vrm=1.
# Usage: sudo bash install.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
INJECT_SRC="$REPO/nct6687_vrm_dkms_inject.py"

if [[ "$(id -u)" -ne 0 ]]; then
	echo "Need root: sudo bash $0" >&2
	exit 1
fi
if [[ ! -f "$INJECT_SRC" ]]; then
	echo "Missing inject script: $INJECT_SRC" >&2
	exit 1
fi

INC_SRC="$REPO/dkms/nct6687_vrm.inc.c"
if [[ ! -f "$INC_SRC" ]]; then
	echo "Missing VRM include: $INC_SRC" >&2
	exit 1
fi

install -d /usr/local/lib/nct6687-vrm
install -m 644 "$INJECT_SRC" /usr/local/lib/nct6687-vrm/nct6687_vrm_dkms_inject.py
install -m 644 "$INC_SRC" /usr/local/lib/nct6687-vrm/nct6687_vrm.inc.c
install -m 755 "$ROOT/nct6687-vrm-reinject" /usr/local/sbin/nct6687-vrm-reinject
install -d /etc/pacman.d/hooks
install -m 644 "$ROOT/nct6687-vrm-reinject.hook" /etc/pacman.d/hooks/nct6687-vrm-reinject.hook
install -m 644 "$ROOT/nct6687-vrm.conf" /etc/modprobe.d/nct6687-vrm.conf

echo "Installed:"
echo "  /usr/local/lib/nct6687-vrm/nct6687_vrm_dkms_inject.py"
echo "  /usr/local/lib/nct6687-vrm/nct6687_vrm.inc.c"
echo "  /usr/local/sbin/nct6687-vrm-reinject"
echo "  /etc/pacman.d/hooks/nct6687-vrm-reinject.hook"
echo "  /etc/modprobe.d/nct6687-vrm.conf  (options nct6687 vrm=1)"
echo
echo "After git pull / inject edits, re-run this script to refresh /usr/local."
echo "Smoke test (rebuild, no unload): sudo /usr/local/sbin/nct6687-vrm-reinject"

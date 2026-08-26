#!/usr/bin/env bash
# Install pacman hooks + stable inject copy + modprobe.d vrm=1.
# Usage: sudo bash install.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
INJECT_SRC="$REPO/nct6687_vrm_dkms_inject.py"
LIB=/usr/local/lib/nct6687-vrm

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

install -d "$LIB"
install -m 644 "$INJECT_SRC" "$LIB/nct6687_vrm_dkms_inject.py"
install -m 644 "$INC_SRC" "$LIB/nct6687_vrm.inc.c"
install -m 644 "$ROOT/refresh-from-source.sh" "$LIB/refresh-from-source.sh"
printf 'SOURCE_REPO=%s\n' "$REPO" >"$LIB/source.env"
chmod 644 "$LIB/source.env"
install -m 755 "$ROOT/nct6687-vrm-reinject" /usr/local/sbin/nct6687-vrm-reinject
install -m 755 "$ROOT/nct6687-vrm-preupgrade" /usr/local/sbin/nct6687-vrm-preupgrade
install -d /etc/pacman.d/hooks
install -m 644 "$ROOT/nct6687-vrm-reinject.hook" /etc/pacman.d/hooks/nct6687-vrm-reinject.hook
install -m 644 "$ROOT/nct6687-vrm-preupgrade.hook" /etc/pacman.d/hooks/nct6687-vrm-preupgrade.hook
install -m 644 "$ROOT/nct6687-vrm.conf" /etc/modprobe.d/nct6687-vrm.conf

echo "Installed:"
echo "  $LIB/nct6687_vrm_dkms_inject.py"
echo "  $LIB/nct6687_vrm.inc.c"
echo "  $LIB/refresh-from-source.sh"
echo "  $LIB/source.env  (SOURCE_REPO=$REPO)"
echo "  /usr/local/sbin/nct6687-vrm-reinject"
echo "  /usr/local/sbin/nct6687-vrm-preupgrade"
echo "  /etc/pacman.d/hooks/nct6687-vrm-reinject.hook"
echo "  /etc/pacman.d/hooks/nct6687-vrm-preupgrade.hook"
echo "  /etc/modprobe.d/nct6687-vrm.conf  (options nct6687 vrm=1)"
echo
echo "The pre-upgrade hook deletes unowned files in /usr/src/nct6687d*"
echo "so leftover extras (Kbuild, *.pre-vrm, the VRM include) cannot block pacman."
echo "The post-upgrade hook refreshes /usr/local from SOURCE_REPO when that"
echo "checkout still exists, then re-injects."
echo
echo "Smoke test: sudo python3 $INJECT_SRC --check"
echo "            sudo /usr/local/sbin/nct6687-vrm-reinject"

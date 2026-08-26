# Sourced by the /usr/local/sbin helpers. Refreshes the installed inject
# copy from the git checkout recorded in source.env (if it still exists).
# Trust model: same as re-running install.sh — the recorded path is
# user-writable.

nct6687_vrm_refresh_from_source() {
	local lib=/usr/local/lib/nct6687-vrm
	local envf="$lib/source.env"
	local tag="${LOG_TAG:-nct6687-vrm}"
	local repo="" src_py src_inc

	[[ -f "$envf" ]] || return 0
	repo=$(sed -n 's/^SOURCE_REPO=//p' "$envf" | tail -n1)
	[[ -n "$repo" && -d "$repo" ]] || return 0

	src_py="$repo/nct6687_vrm_dkms_inject.py"
	src_inc="$repo/dkms/nct6687_vrm.inc.c"
	[[ -f "$src_py" && -f "$src_inc" ]] || return 0

	if cmp -s "$src_py" "$lib/nct6687_vrm_dkms_inject.py" \
		&& cmp -s "$src_inc" "$lib/nct6687_vrm.inc.c"; then
		return 0
	fi

	echo "$tag: refreshing /usr/local from $repo" >&2
	install -m 644 "$src_py" "$lib/nct6687_vrm_dkms_inject.py"
	install -m 644 "$src_inc" "$lib/nct6687_vrm.inc.c"
	if command -v logger >/dev/null 2>&1; then
		logger -t "$tag" "refreshed installed inject from $repo"
	fi
}

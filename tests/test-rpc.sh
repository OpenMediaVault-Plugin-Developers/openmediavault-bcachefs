#!/usr/bin/env bash
# test-rpc.sh — Integration tests for openmediavault-bcachefs RPC methods.
#
# Usage: sudo ./tests/test-rpc.sh /dev/sdX [/dev/sdY ...]
#
# Formats the given device(s) as a bcachefs filesystem, mounts it at the
# standard OMV path, exercises every plugin RPC method, then cleans up.
#
# WARNING: All supplied devices will be wiped.

set -uo pipefail

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
if [ $# -lt 1 ]; then
    echo "Usage: $(basename "$0") /dev/sdX [/dev/sdY ...]" >&2
    exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
    echo "Must be run as root." >&2
    exit 1
fi

# Canonicalise every device argument. OMV's storage layer only recognises
# device-mapper nodes as /dev/mapper/* or /dev/dm-N — the /dev/<vg>/<lv>
# symlink form that LVM also creates is not matched by any storage backend,
# so createFilesystem (and every other RPC that calls assertGetStorageDevice)
# fails with "Device '...' does not exist." Resolving each argument with
# `readlink -f` turns LVM logical volumes into /dev/dm-N and leaves plain
# disks like /dev/sdX untouched.
DEVICES=()
for _dev in "$@"; do
    if [ ! -b "$_dev" ]; then
        echo "Not a block device: $_dev" >&2
        exit 1
    fi
    _real=$(readlink -f "$_dev")
    [ "$_real" != "$_dev" ] && echo "  resolved $_dev -> $_real" >&2
    DEVICES+=("$_real")
done

# ---------------------------------------------------------------------------
# Colours / counters  (all display output goes to stderr so that $() captures
# only the pure JSON returned by assert_rpc / rpc)
# ---------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

PASS=0
FAIL=0
declare -a FAILED_TESTS=()

section() { echo -e "\n${CYAN}${BOLD}=== $* ===${NC}" >&2; }
info()    { echo -e "  ${YELLOW}»${NC} $*" >&2; }

_pass() {
    echo -e "  ${GREEN}PASS${NC}  $1" >&2
    ((PASS++)) || true
}
_fail() {
    echo -e "  ${RED}FAIL${NC}  $1" >&2
    [ -n "${2:-}" ] && echo -e "         ${RED}→${NC} $2" >&2
    ((FAIL++)) || true
    FAILED_TESTS+=("$1")
}

# ---------------------------------------------------------------------------
# RPC helpers
# ---------------------------------------------------------------------------

# Call an RPC and return raw JSON on stdout; exit non-zero on error.
rpc() {
    local svc=$1 method=$2 params=${3:-'{}'}
    omv-rpc -u admin "$svc" "$method" "$params"
}

# Assert an RPC succeeds. Optional 5th arg: grep pattern.
# Prints raw JSON to stdout (for capture with $()).
assert_rpc() {
    local desc=$1 svc=$2 method=$3 params=${4:-'{}'} pattern=${5:-}
    local out ec=0
    out=$(omv-rpc -u admin "$svc" "$method" "$params" 2>&1) || ec=$?
    if [ $ec -ne 0 ]; then
        _fail "$desc" "$(echo "$out" | tail -3)"
        return 1
    fi
    if [ -n "$pattern" ] && ! echo "$out" | grep -q "$pattern"; then
        _fail "$desc" "Pattern '$pattern' not found in output: ${out:0:200}"
        return 1
    fi
    _pass "$desc"
    echo "$out"   # stdout only — captured cleanly by $()
    return 0
}

# Assert an RPC fails (non-zero exit or output contains Exception).
assert_rpc_fails() {
    local desc=$1 svc=$2 method=$3 params=${4:-'{}'}
    local out ec=0
    out=$(omv-rpc -u admin "$svc" "$method" "$params" 2>&1) || ec=$?
    if [ $ec -eq 0 ] && ! echo "$out" | grep -qi "exception"; then
        _fail "$desc" "Expected failure but RPC succeeded: ${out:0:200}"
        return 1
    fi
    _pass "$desc"
    return 0
}

# Call a *Bg method and wait for the background task.
assert_rpc_bg() {
    local desc=$1 svc=$2 method=$3 params=${4:-'{}'}
    local filename ec=0
    filename=$(omv-rpc -u admin "$svc" "$method" "$params" 2>&1) || ec=$?
    if [ $ec -ne 0 ]; then
        _fail "$desc" "Failed to start bg task: ${filename:0:200}"
        return 1
    fi
    filename=$(echo "$filename" | tr -d '"')

    local timeout=180 elapsed=0 poll_ec poll_out
    while [ $elapsed -lt $timeout ]; do
        poll_out=$(omv-rpc -u admin "Exec" "isRunning" \
            "{\"filename\":\"$filename\"}" 2>&1)
        poll_ec=$?
        [ $poll_ec -ne 0 ] && break
        echo "$poll_out" | grep -q '"running":true\|"running": true' || break
        sleep 2; ((elapsed += 2)) || true
    done
    if [ $elapsed -ge $timeout ]; then
        _fail "$desc" "Bg task timed out after ${timeout}s"
        return 1
    fi

    if [ $poll_ec -ne 0 ]; then
        local err
        err=$(echo "$poll_out" | python3 -c \
            "import sys,json
d=json.load(sys.stdin)
e=d.get('error') or {}
print(e.get('message', str(d))[:300])" 2>/dev/null \
            || echo "${poll_out:0:200}")
        _fail "$desc" "$err"
        return 1
    fi

    local content
    content=$(omv-rpc -u admin "Exec" "getOutput" \
        "{\"filename\":\"$filename\",\"pos\":0}" 2>/dev/null \
        | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('output',''))" \
        2>/dev/null || echo "")
    if echo "$content" | grep -q "Exception"; then
        _fail "$desc" "$(echo "$content" | grep "Exception" | head -2)"
        return 1
    fi

    _pass "$desc"
    return 0
}

# Print one field of this filesystem's getDeviceList row for a device,
# matched by resolved path (rows may use /dev/mapper/* or by-id names).
dev_field() {
    local dev=$1 field=$2
    rpc "Bcachefs" "getDeviceList" "{}" 2>/dev/null | python3 -c "
import sys, json, os
for r in json.load(sys.stdin):
    d = r.get('devicefile') or ''
    if r.get('filesystem') == '$FS_UUID' and d and \
            os.path.realpath(d) == os.path.realpath('$dev'):
        v = r.get('$field')
        print(v if isinstance(v, str) else json.dumps(v))
        break
" 2>/dev/null
}

# Print one field of this filesystem's getDeviceList row by member slot.
slot_field() {
    local slot=$1 field=$2
    rpc "Bcachefs" "getDeviceList" "{}" 2>/dev/null | python3 -c "
import sys, json
for r in json.load(sys.stdin):
    if r.get('filesystem') == '$FS_UUID' and r.get('slot') == $slot:
        v = r.get('$field')
        print(v if isinstance(v, str) else json.dumps(v))
        break
" 2>/dev/null
}

# Assert two values are equal.
assert_eq() {
    local desc=$1 expected=$2 actual=$3
    if [ "$expected" = "$actual" ]; then
        _pass "$desc"
    else
        _fail "$desc" "expected '$expected', got '$actual'"
    fi
}

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------
section "Pre-flight checks"

for cmd in omv-rpc bcachefs python3 wipefs; do
    if command -v "$cmd" &>/dev/null; then
        _pass "command available: $cmd"
    else
        _fail "command available: $cmd" "$cmd not found"
    fi
done

if ! omv-rpc -u admin "Config" "isDirty" '{}' &>/dev/null; then
    echo -e "\n${RED}omv-rpc not functional — aborting.${NC}" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# State shared across sections
# ---------------------------------------------------------------------------
FS_UUID=""
MNT=""
SNAP_JOB_UUID=""
SCRUB_JOB_UUID=""
SUBVOL_PATH=""
LIST_PARAMS='{"start":0,"limit":null,"sortfield":null,"sortdir":null}'

OMV_NEW_UUID=$(. /etc/default/openmediavault 2>/dev/null; \
    echo "${OMV_CONFIGOBJECT_NEW_UUID:-fa4b1c66-ef79-11e5-87a0-0002b3a176b4}")

# ---------------------------------------------------------------------------
# Cleanup — always runs on exit
# ---------------------------------------------------------------------------
cleanup() {
    section "Cleanup"

    # Delete any test snapshot jobs left in the confdb.
    local j_uuid
    for j_uuid in $(rpc "Bcachefs" "getSnapshotJobList" "$LIST_PARAMS" 2>/dev/null \
            | python3 -c "
import sys,json
for j in json.load(sys.stdin).get('data',[]):
    print(j['uuid'])" 2>/dev/null || true); do
        info "Deleting snapshot job $j_uuid"
        rpc "Bcachefs" "deleteSnapshotJob" "{\"uuid\":\"$j_uuid\"}" &>/dev/null || true
    done

    # Delete any test scrub jobs left in the confdb.
    for j_uuid in $(rpc "Bcachefs" "getScrubJobList" "$LIST_PARAMS" 2>/dev/null \
            | python3 -c "
import sys,json
for j in json.load(sys.stdin).get('data',[]):
    print(j['uuid'])" 2>/dev/null || true); do
        info "Deleting scrub job $j_uuid"
        rpc "Bcachefs" "deleteScrubJob" "{\"uuid\":\"$j_uuid\"}" &>/dev/null || true
    done

    if [ -n "$MNT" ] && mountpoint -q "$MNT" 2>/dev/null; then
        info "Unmounting $MNT"
        umount -l "$MNT" 2>/dev/null || true
    fi
    if [ -n "$MNT" ] && [ -d "$MNT" ]; then
        info "Removing mount point $MNT"
        rmdir "$MNT" 2>/dev/null || true
    fi
    if [ -n "$FS_UUID" ]; then
        info "Removing auto-unlock key file (if any)"
        rm -f "/etc/bcachefs/keys/${FS_UUID}.key" 2>/dev/null || true
    fi
    info "Wiping devices"
    for dev in "${DEVICES[@]}"; do
        wipefs -a "$dev" 2>/dev/null || true
    done
    info "Done."

    # Deploy pending config changes so the OMV web UI "apply changes" banner
    # does not linger after this test run. Runs detached/async so the script
    # returns promptly; --append-dirty clears the dirty-module markers (the
    # banner) once the deploy completes.
    info "Deploying pending config changes asynchronously (clears web UI banner)"
    nohup omv-salt deploy run --quiet --append-dirty >/dev/null 2>&1 &
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
section "Configuration"
info "Devices : ${DEVICES[*]}"
info "Count   : ${#DEVICES[@]}"

DEVICEFILES_CSV=$(IFS=,; echo "${DEVICES[*]}")

# ===========================================================================
section "Informational RPCs (no filesystem required)"
# ===========================================================================

assert_rpc "getSettings"             "Bcachefs" "getSettings"       "{}" "suite"
assert_rpc "getSubvolumes (empty)"   "Bcachefs" "getSubvolumes"      "{}"
assert_rpc "getFilesystems (empty)"  "Bcachefs" "getFilesystems"     "{}"
assert_rpc "getFilesystemList (empty)" "Bcachefs" "getFilesystemList" "$LIST_PARAMS"
assert_rpc "getSubvolumeList (empty)"  "Bcachefs" "getSubvolumeList"  "{}"
assert_rpc "getDeviceList (empty)"     "Bcachefs" "getDeviceList"      "{}"
assert_rpc "getSnapshotJobList (empty)" "Bcachefs" "getSnapshotJobList" "$LIST_PARAMS"
assert_rpc "getScrubJobList (empty)"    "Bcachefs" "getScrubJobList"    "$LIST_PARAMS"

# ===========================================================================
section "Settings — read/write"
# ===========================================================================

ORIG_SUITE=$(rpc "Bcachefs" "getSettings" "{}" 2>/dev/null \
    | python3 -c "import sys,json; print(json.load(sys.stdin).get('suite',''))" 2>/dev/null \
    || echo "bcachefs-tools-release")
info "Current suite: $ORIG_SUITE"

assert_rpc "setSettings — snapshot suite" "Bcachefs" "setSettings" \
    '{"suite":"bcachefs-tools-snapshot"}'
assert_rpc "setSettings — release suite"  "Bcachefs" "setSettings" \
    '{"suite":"bcachefs-tools-release"}'
assert_rpc_fails "setSettings — invalid suite" "Bcachefs" "setSettings" \
    '{"suite":"bcachefs-tools-invalid"}'

rpc "Bcachefs" "setSettings" "{\"suite\":\"$ORIG_SUITE\"}" &>/dev/null || true

# ===========================================================================
section "Filesystem — create (background task)"
# ===========================================================================

CREATE_PARAMS=$(python3 -c "
import json
print(json.dumps({
    'label':       'omvbcachefstest',
    'replicas':    1,
    'compression': 'lz4',
    'checksum':    'crc32c',
    'erasurecode': False,
    'nocow':       False,
    'encrypted':   False,
    'passphrase':  '',
    'autounlock':  False,
    'devicefiles': '$DEVICEFILES_CSV',
}))
")

assert_rpc_bg "createFilesystem" "Bcachefs" "createFilesystem" "$CREATE_PARAMS"

info "Reading filesystem UUID from superblock ..."
SUPER_OUT=$(/usr/sbin/bcachefs show-super "${DEVICES[0]}" 2>/dev/null || true)
FS_UUID=$(echo "$SUPER_OUT" | awk '/^External UUID:/ {print $NF}')

if [ -n "$FS_UUID" ]; then
    _pass "createFilesystem — UUID detected: $FS_UUID"
else
    _fail "createFilesystem — could not read UUID from superblock"
    echo -e "\n${RED}Cannot continue without a filesystem UUID.${NC}" >&2
    exit 1
fi

FS_LABEL=$(echo "$SUPER_OUT" | awk '/^Label:/ {print $NF}')
if [ "$FS_LABEL" = "omvbcachefstest" ]; then
    _pass "createFilesystem — label 'omvbcachefstest' in superblock"
else
    _fail "createFilesystem — expected label 'omvbcachefstest', got '$FS_LABEL'"
fi

if echo "$SUPER_OUT" | grep -q "compression:.*lz4"; then
    _pass "createFilesystem — compression=lz4 in superblock"
else
    _fail "createFilesystem — compression=lz4 not found in superblock"
fi

# ===========================================================================
section "Filesystem — mount at OMV standard path"
# ===========================================================================

MNT="/srv/dev-disk-by-uuid-${FS_UUID}"
info "Mount point: $MNT"

mkdir -p "$MNT"
# Multi-device bcachefs mounts take a colon-separated SOURCE (this is also
# what OMV writes to fstab). Mounting a single member would attach only that
# device, so getDeviceList / getMountedBcachefsFilesystems would then see
# just one device instead of the whole array.
MOUNT_SRC=$(IFS=:; echo "${DEVICES[*]}")
if mount -t bcachefs "$MOUNT_SRC" "$MNT" 2>/dev/null; then
    _pass "mount bcachefs at $MNT"
else
    _fail "mount bcachefs at $MNT" "mount failed"
    exit 1
fi

# ===========================================================================
section "Filesystem — list and details (filesystem mounted)"
# ===========================================================================

# Use rpc() for pure JSON so we can parse it.
FS_LIST=$(rpc "Bcachefs" "getFilesystemList" "$LIST_PARAMS" 2>/dev/null || echo "{}")

if echo "$FS_LIST" | grep -q "$FS_UUID"; then
    _pass "getFilesystemList — filesystem present"
else
    _fail "getFilesystemList — filesystem $FS_UUID not found"
fi

if echo "$FS_LIST" | grep -q "omvbcachefstest"; then
    _pass "getFilesystemList — label 'omvbcachefstest' returned"
else
    _fail "getFilesystemList — label not found (show-super may have failed)"
fi

if echo "$FS_LIST" | grep -q '"autounlock":false'; then
    _pass "getFilesystemList — autounlock starts false"
else
    _fail "getFilesystemList — expected autounlock=false"
fi

# Regression: encryption used to be read from the Features line, where
# bcachefs never lists it. An unencrypted filesystem must still read false
# (the encrypted case is covered in its own section below).
if echo "$FS_LIST" | grep -q '"encrypted":false'; then
    _pass "getFilesystemList — unencrypted filesystem reports encrypted=false"
else
    _fail "getFilesystemList — expected encrypted=false"
fi

# Separately test via assert_rpc for pass/fail tracking.
assert_rpc "getFilesystemList (assert)"  "Bcachefs" "getFilesystemList" \
    "$LIST_PARAMS" "$FS_UUID" >/dev/null
assert_rpc "getFilesystem"   "Bcachefs" "getFilesystem" \
    "{\"uuid\":\"$FS_UUID\"}" "$FS_UUID" >/dev/null
assert_rpc "getFilesystems"  "Bcachefs" "getFilesystems" "{}" "$FS_UUID" >/dev/null

# getSubvolumes: grep for just the UUID (no slashes that would be JSON-escaped)
FS_UUID_SHORT="${FS_UUID:0:8}"
assert_rpc "getSubvolumes"    "Bcachefs" "getSubvolumes"  "{}" "$FS_UUID_SHORT" >/dev/null
assert_rpc "getSubvolumeList" "Bcachefs" "getSubvolumeList" "{}" "root" >/dev/null

assert_rpc_fails "getFilesystem — unknown UUID" "Bcachefs" "getFilesystem" \
    '{"uuid":"00000000-0000-0000-0000-000000000000"}'

# ===========================================================================
section "Filesystem — usage, options & device list (read)"
# ===========================================================================

assert_rpc "getUsage" "Bcachefs" "getUsage" \
    "{\"uuid\":\"$FS_UUID\"}" "$FS_UUID" >/dev/null
assert_rpc "getFilesystemOptions" "Bcachefs" "getFilesystemOptions" \
    "{\"uuid\":\"$FS_UUID\"}" "compression" >/dev/null
assert_rpc "getDeviceList" "Bcachefs" "getDeviceList" \
    "{}" "$FS_UUID" >/dev/null

# Verify the device list reports exactly the devices that make up the fs.
DEV_LIST=$(rpc "Bcachefs" "getDeviceList" "{}" 2>/dev/null || echo "[]")
DEV_COUNT=$(echo "$DEV_LIST" | python3 -c "
import sys,json
rows=json.load(sys.stdin)
print(sum(1 for r in rows if r.get('filesystem') == '$FS_UUID'))" 2>/dev/null || echo 0)
if [ "$DEV_COUNT" -eq "${#DEVICES[@]}" ]; then
    _pass "getDeviceList — reports ${#DEVICES[@]} device(s) for filesystem"
else
    _fail "getDeviceList — expected ${#DEVICES[@]} device(s), got $DEV_COUNT"
fi

assert_rpc_fails "getUsage — unknown UUID" "Bcachefs" "getUsage" \
    '{"uuid":"00000000-0000-0000-0000-000000000000"}'

# Failure domains: none configured yet, so every device is its own domain.
# The filesystem has a single replica, so losing any device loses data and
# separation is (correctly) reported as violated.
assert_rpc "getUsage — includes failure domain report" "Bcachefs" "getUsage" \
    "{\"uuid\":\"$FS_UUID\"}" '"failuredomains"' >/dev/null
if /usr/sbin/bcachefs fs failure-domains --help &>/dev/null; then
    FD_ERR=$(mktemp)
    if rpc "Bcachefs" "getFailureDomains" "{\"uuid\":\"$FS_UUID\"}" 2>&1 | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert len(d['domains']) == ${#DEVICES[@]}, 'expected one domain per device: %r' % d
assert all(x['devices'] == 1 for x in d['domains']), d
lost = any(x['lost_bytes'] > 0 for x in d['domains'])
assert d['separation_violated'] == lost, 'violation flag disagrees with lost data: %r' % d
" 2>"$FD_ERR"; then
        _pass "getFailureDomains — one domain per device, violation flag consistent"
    else
        _fail "getFailureDomains — unexpected result" "$(tail -1 "$FD_ERR")"
    fi
    rm -f "$FD_ERR"
    assert_rpc_fails "getFailureDomains — unknown UUID" "Bcachefs" "getFailureDomains" \
        '{"uuid":"00000000-0000-0000-0000-000000000000"}'
else
    info "bcachefs-tools has no 'fs failure-domains'; skipping getFailureDomains"
fi

# New device list fields.
assert_eq "getDeviceList — device is online" "true" "$(dev_field "${DEVICES[0]}" online)"
assert_eq "getDeviceList — no failure domain set" "" "$(dev_field "${DEVICES[0]}" failure_domain)"
FIRST_SLOT=$(dev_field "${DEVICES[0]}" slot)
if [[ "$FIRST_SLOT" =~ ^[0-9]+$ ]]; then
    _pass "getDeviceList — reports member slot ($FIRST_SLOT)"
else
    _fail "getDeviceList — no member slot" "got '$FIRST_SLOT'"
fi

# setFilesystemOptions writes to sysfs; only available on kernels that expose
# the per-filesystem options tree.
OPTS_DIR="/sys/fs/bcachefs/${FS_UUID}/options"
if [ -d "$OPTS_DIR" ]; then
    assert_rpc "setFilesystemOptions — compression=zstd" "Bcachefs" \
        "setFilesystemOptions" \
        "{\"uuid\":\"$FS_UUID\",\"compression\":\"zstd\"}" >/dev/null
    assert_rpc "getFilesystemOptions — compression now zstd" "Bcachefs" \
        "getFilesystemOptions" "{\"uuid\":\"$FS_UUID\"}" "zstd" >/dev/null
    # Restore the original compression so later assertions are unaffected.
    rpc "Bcachefs" "setFilesystemOptions" \
        "{\"uuid\":\"$FS_UUID\",\"compression\":\"lz4\"}" &>/dev/null || true

    # Degraded-write policy and missing-device timeout. sysfs prints choice
    # options as "[degraded] yes no"; the RPC must return just the choice.
    OPTS_JSON=$(rpc "Bcachefs" "getFilesystemOptions" "{\"uuid\":\"$FS_UUID\"}" 2>/dev/null)
    OPT_WD=$(echo "$OPTS_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin).get('write_degraded'))" 2>/dev/null)
    OPT_MDT=$(echo "$OPTS_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin).get('missing_dev_timeout'))" 2>/dev/null)
    if [[ "$OPT_WD" =~ ^(degraded|yes|no)$ ]]; then
        _pass "getFilesystemOptions — write_degraded is a plain choice ($OPT_WD)"
    else
        _fail "getFilesystemOptions — write_degraded not parsed" "got '$OPT_WD'"
    fi
    assert_rpc "setFilesystemOptions — write_degraded=no, missing_dev_timeout=30" \
        "Bcachefs" "setFilesystemOptions" \
        "{\"uuid\":\"$FS_UUID\",\"write_degraded\":\"no\",\"missing_dev_timeout\":30}" >/dev/null
    OPTS_JSON=$(rpc "Bcachefs" "getFilesystemOptions" "{\"uuid\":\"$FS_UUID\"}" 2>/dev/null)
    assert_eq "getFilesystemOptions — write_degraded now no" "no" \
        "$(echo "$OPTS_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin).get('write_degraded'))" 2>/dev/null)"
    assert_eq "getFilesystemOptions — missing_dev_timeout now 30" "30" \
        "$(echo "$OPTS_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin).get('missing_dev_timeout'))" 2>/dev/null)"
    rpc "Bcachefs" "setFilesystemOptions" \
        "{\"uuid\":\"$FS_UUID\",\"write_degraded\":\"$OPT_WD\",\"missing_dev_timeout\":${OPT_MDT:-0}}" &>/dev/null || true
    assert_rpc_fails "setFilesystemOptions — invalid write_degraded" "Bcachefs" \
        "setFilesystemOptions" "{\"uuid\":\"$FS_UUID\",\"write_degraded\":\"sometimes\"}"
    assert_rpc_fails "setFilesystemOptions — target with shell characters" "Bcachefs" \
        "setFilesystemOptions" "{\"uuid\":\"$FS_UUID\",\"foreground_target\":\"ssd;true\"}"
else
    info "Skipping setFilesystemOptions — $OPTS_DIR not present"
fi

# ===========================================================================
section "Filesystem — grow (core Filesystems page)"
# ===========================================================================

# The backend advertises online resize, so OMV offers Grow. It used to fall
# through to a no-op; it now resizes members whose device has grown. With
# nothing to grow it must succeed without changing anything.
assert_rpc "FileSystemMgmt grow — nothing to grow" "FileSystemMgmt" "grow" \
    "{\"id\":\"$FS_UUID\"}" >/dev/null

# Exercise a real grow when the first test device is an LVM volume with
# free space in its volume group.
GROW_SLOT=$(dev_field "${DEVICES[0]}" slot)
GROW_DM=$(dmsetup info -c --noheadings -o name "${DEVICES[0]}" 2>/dev/null | tr -d ' ')
if [ -n "$GROW_DM" ] && [[ "$GROW_SLOT" =~ ^[0-9]+$ ]] && \
        lvs "/dev/mapper/$GROW_DM" &>/dev/null && \
        lvextend -q -L +256M "/dev/mapper/$GROW_DM" &>/dev/null; then
    NB_FILE="/sys/fs/bcachefs/${FS_UUID}/dev-${GROW_SLOT}/nbuckets"
    NB_BEFORE=$(cat "$NB_FILE")
    assert_rpc "FileSystemMgmt grow — after lvextend" "FileSystemMgmt" "grow" \
        "{\"id\":\"$FS_UUID\"}" >/dev/null
    NB_AFTER=$(cat "$NB_FILE")
    if [ "$NB_AFTER" -gt "$NB_BEFORE" ]; then
        _pass "grow — member grew from $NB_BEFORE to $NB_AFTER buckets"
    else
        _fail "grow — member still has $NB_AFTER buckets (was $NB_BEFORE)"
    fi
else
    info "Skipping real grow — ${DEVICES[0]} is not an LVM volume with free VG space"
fi

# ===========================================================================
section "Damage tracking"
# ===========================================================================

# A freshly formatted filesystem has no damage records, and producing real
# damage needs a failed repair, so these cover the RPC plumbing, the empty
# case and path validation. Parsing of populated output is covered by
# tests/test-parsers.php.
if /usr/sbin/bcachefs damage --help &>/dev/null; then
    DAMAGE_FILE="damage-test.txt"
    echo "damage test" > "$MNT/$DAMAGE_FILE"
    sync

    DAMAGE_LIST=$(assert_rpc "getDamageList" "Bcachefs" "getDamageList" \
        "{\"uuid\":\"$FS_UUID\"}")
    if [ "$(echo "$DAMAGE_LIST" | python3 -c \
            'import sys,json; print(len(json.load(sys.stdin)))' 2>/dev/null)" = "0" ]; then
        _pass "getDamageList — fresh filesystem has no damaged files"
    else
        _fail "getDamageList — expected an empty list" "${DAMAGE_LIST:0:200}"
    fi

    assert_rpc_bg "getDamageDetails — undamaged file" "Bcachefs" "getDamageDetails" \
        "{\"uuid\":\"$FS_UUID\",\"path\":\"$DAMAGE_FILE\"}"
    assert_rpc "clearDamage — undamaged file" "Bcachefs" "clearDamage" \
        "{\"uuid\":\"$FS_UUID\",\"path\":\"$DAMAGE_FILE\"}" >/dev/null
    assert_rpc "clearAllDamage" "Bcachefs" "clearAllDamage" \
        "{\"uuid\":\"$FS_UUID\"}" >/dev/null

    assert_rpc_fails "getDamageList — unknown UUID" "Bcachefs" "getDamageList" \
        '{"uuid":"00000000-0000-0000-0000-000000000000"}'
    assert_rpc_fails "clearDamage — missing path" "Bcachefs" "clearDamage" \
        "{\"uuid\":\"$FS_UUID\"}"
    assert_rpc_fails "clearDamage — empty path" "Bcachefs" "clearDamage" \
        "{\"uuid\":\"$FS_UUID\",\"path\":\"\"}"
    assert_rpc_fails "clearDamage — nonexistent path" "Bcachefs" "clearDamage" \
        "{\"uuid\":\"$FS_UUID\",\"path\":\"no-such-file\"}"
    assert_rpc_fails "clearDamage — '..' escapes the filesystem" "Bcachefs" \
        "clearDamage" "{\"uuid\":\"$FS_UUID\",\"path\":\"../../etc/hostname\"}"
    ln -sfn /etc "$MNT/damage-escape"
    assert_rpc_fails "clearDamage — symlink escapes the filesystem" "Bcachefs" \
        "clearDamage" "{\"uuid\":\"$FS_UUID\",\"path\":\"damage-escape/hostname\"}"
    assert_rpc_fails "getDamageDetails — symlink escapes the filesystem" "Bcachefs" \
        "getDamageDetails" "{\"uuid\":\"$FS_UUID\",\"path\":\"damage-escape/hostname\"}"

    rm -f "$MNT/damage-escape" "$MNT/$DAMAGE_FILE"
else
    info "bcachefs-tools has no 'damage' command (needs 1.39.5+)"
    assert_rpc_fails "getDamageList — unsupported tools report an error" \
        "Bcachefs" "getDamageList" "{\"uuid\":\"$FS_UUID\"}"
fi

# ===========================================================================
section "Subvolumes — create"
# ===========================================================================

SUBVOL_PATH="${MNT}/testsubvol"

assert_rpc "createSubvolume" "Bcachefs" "createSubvolume" \
    "{\"parent\":\"$MNT\",\"name\":\"testsubvol\"}" >/dev/null

if [ -d "$SUBVOL_PATH" ]; then
    _pass "createSubvolume — directory exists: $SUBVOL_PATH"
else
    _fail "createSubvolume — directory not found: $SUBVOL_PATH"
fi

# Parse getSubvolumeList via rpc() for clean JSON.
# Note: bcachefs subvolume list --json returns [] on this kernel/tools version
# (ioctl succeeds but reports no entries), so scandir fallback never fires.
# Verify that the test filesystem root entry is present and marked inuse=true.
SV_LIST=$(rpc "Bcachefs" "getSubvolumeList" "{}" 2>/dev/null || echo "[]")
if echo "$SV_LIST" | python3 -c "
import sys,json
rows=json.load(sys.stdin)
root=next((r for r in rows if r.get('filesystem','') == '$FS_UUID'
           and r.get('relpath','') == '(root)'), None)
assert root is not None, 'test filesystem root not found'
assert root.get('inuse') == True, 'expected root inuse=true'
assert 'usage' in root, 'expected usage field present'
" 2>/dev/null; then
    _pass "getSubvolumeList — test filesystem root present, inuse=true, has usage"
else
    _fail "getSubvolumeList — test filesystem root not found / missing usage field"
fi

assert_rpc "getSubvolumes (with subvol)" "Bcachefs" "getSubvolumes" \
    "{}" "$FS_UUID_SHORT" >/dev/null

assert_rpc_fails "createSubvolume — slash in name" "Bcachefs" "createSubvolume" \
    "{\"parent\":\"$MNT\",\"name\":\"bad/name\"}"

# The subvolume must be listed from the real `subvolume list --json`
# output, as a subvolume and not a snapshot. This is what checks the JSON
# normaliser against the installed bcachefs-tools.
SV_ERR=$(mktemp)
if rpc "Bcachefs" "getSubvolumeList" "{}" 2>/dev/null | python3 -c "
import sys, json
rows = [r for r in json.load(sys.stdin) if r.get('filesystem') == '$FS_UUID']
sv = next((r for r in rows if r.get('relpath') == 'testsubvol'), None)
assert sv is not None, 'testsubvol not listed; rows: %r' % rows
assert sv['issnapshot'] is False, 'testsubvol listed as a snapshot: %r' % sv
assert sv['path'] == '$SUBVOL_PATH', sv
" 2>"$SV_ERR"; then
    _pass "getSubvolumeList — new subvolume listed, not a snapshot"
else
    _fail "getSubvolumeList — subvolume parsing" "$(tail -1 "$SV_ERR")"
fi
rm -f "$SV_ERR"

if php -r '
    require_once "openmediavault/autoloader.inc";
    require_once "openmediavault/globals.inc";
    require_once "openmediavault/functions.inc";
    exit(\OMV\System\Filesystem\Bcachefs::isSubvolume($argv[1]) ? 0 : 1);
' "$SUBVOL_PATH" 2>/dev/null; then
    _pass "Bcachefs::isSubvolume — true for a subvolume"
else
    _fail "Bcachefs::isSubvolume — false for $SUBVOL_PATH"
fi

# ===========================================================================
section "Subvolumes — nested subvolume"
# ===========================================================================

assert_rpc "createSubvolume (nested)" "Bcachefs" "createSubvolume" \
    "{\"parent\":\"$SUBVOL_PATH\",\"name\":\"nested\"}" >/dev/null

if [ -d "${SUBVOL_PATH}/nested" ]; then
    _pass "createSubvolume (nested) — directory exists"
else
    _fail "createSubvolume (nested) — directory not found"
fi

assert_rpc "deleteSubvolume (nested)" "Bcachefs" "deleteSubvolume" \
    "{\"path\":\"${SUBVOL_PATH}/nested\"}" >/dev/null
if [ ! -d "${SUBVOL_PATH}/nested" ]; then
    _pass "deleteSubvolume (nested) — directory removed"
else
    _fail "deleteSubvolume (nested) — directory still exists"
fi

# ===========================================================================
section "Subvolumes — delete refuses anything that is not a subvolume"
# ===========================================================================

# deleteSubvolume used to fall back to `rm -rf` on any path whenever
# `bcachefs subvolume delete` failed. Each of these must fail and leave the
# target untouched.
PLAIN_DIR="${MNT}/plain-dir"
mkdir -p "$PLAIN_DIR" && echo keep > "$PLAIN_DIR/file"
assert_rpc_fails "deleteSubvolume — plain directory is refused" "Bcachefs" \
    "deleteSubvolume" "{\"path\":\"$PLAIN_DIR\"}"
if [ -f "$PLAIN_DIR/file" ]; then
    _pass "deleteSubvolume — plain directory left intact"
else
    _fail "deleteSubvolume — plain directory contents were deleted"
fi
rm -rf "$PLAIN_DIR"

assert_rpc_fails "deleteSubvolume — filesystem root is refused" "Bcachefs" \
    "deleteSubvolume" "{\"path\":\"$MNT\"}"
if mountpoint -q "$MNT" && [ -d "$SUBVOL_PATH" ]; then
    _pass "deleteSubvolume — filesystem root still mounted with its contents"
else
    _fail "deleteSubvolume — filesystem root was affected"
fi

OUTSIDE_DIR=$(mktemp -d)
echo keep > "$OUTSIDE_DIR/file"
assert_rpc_fails "deleteSubvolume — path outside bcachefs is refused" "Bcachefs" \
    "deleteSubvolume" "{\"path\":\"$OUTSIDE_DIR\"}"
if [ -f "$OUTSIDE_DIR/file" ]; then
    _pass "deleteSubvolume — path outside bcachefs left intact"
else
    _fail "deleteSubvolume — path outside bcachefs was deleted"
fi
rm -rf "$OUTSIDE_DIR"

assert_rpc_fails "deleteSubvolume — '..' escape out of the mount" "Bcachefs" \
    "deleteSubvolume" "{\"path\":\"$MNT/../\"}"

# ===========================================================================
section "Snapshot jobs — CRUD"
# ===========================================================================

SET_SNAP_PARAMS=$(python3 -c "
import json
print(json.dumps({
    'uuid':             '$OMV_NEW_UUID',
    'enable':           True,
    'subvolume':        '$MNT',
    'prefix':           'test',
    'retention':        3,
    'retentionunit':    'count',
    'readonly':         False,
    'execution':        'daily',
    'minute':           ['0'],
    'hour':             ['2'],
    'dayofmonth':       ['*'],
    'month':            ['*'],
    'dayofweek':        ['*'],
    'everynminute':     False,
    'everynhour':       False,
    'everyndayofmonth': False,
    'sendemail':        False,
    'emailonerror':     False,
    'comment':          'rpc test job',
}))
")

# Use rpc() directly so we get pure JSON for UUID extraction.
SNAP_RESULT=$(rpc "Bcachefs" "setSnapshotJob" "$SET_SNAP_PARAMS" 2>&1)
SNAP_JOB_UUID=$(echo "$SNAP_RESULT" | python3 -c \
    "import sys,json; print(json.load(sys.stdin).get('uuid',''))" 2>/dev/null || echo "")

if [ -n "$SNAP_JOB_UUID" ] && [ "$SNAP_JOB_UUID" != "$OMV_NEW_UUID" ]; then
    _pass "setSnapshotJob (create) — UUID: $SNAP_JOB_UUID"
else
    _fail "setSnapshotJob (create)" "No UUID in: ${SNAP_RESULT:0:200}"
fi

assert_rpc "getSnapshotJobList" "Bcachefs" "getSnapshotJobList" \
    "$LIST_PARAMS" "rpc test job" >/dev/null

if [ -n "$SNAP_JOB_UUID" ]; then
    assert_rpc "getSnapshotJob" "Bcachefs" "getSnapshotJob" \
        "{\"uuid\":\"$SNAP_JOB_UUID\"}" "rpc test job" >/dev/null

    EDIT_SNAP_PARAMS=$(python3 -c "
import json
print(json.dumps({
    'uuid':             '$SNAP_JOB_UUID',
    'enable':           True,
    'subvolume':        '$MNT',
    'prefix':           'test',
    'retention':        5,
    'retentionunit':    'count',
    'readonly':         True,
    'execution':        'daily',
    'minute':           ['0'],
    'hour':             ['3'],
    'dayofmonth':       ['*'],
    'month':            ['*'],
    'dayofweek':        ['*'],
    'everynminute':     False,
    'everynhour':       False,
    'everyndayofmonth': False,
    'sendemail':        False,
    'emailonerror':     False,
    'comment':          'rpc test job edited',
}))
")
    assert_rpc "setSnapshotJob (edit)" "Bcachefs" "setSnapshotJob" \
        "$EDIT_SNAP_PARAMS" "edited" >/dev/null
fi

# ===========================================================================
section "Snapshot jobs — run"
# ===========================================================================

if [ -n "$SNAP_JOB_UUID" ]; then
    assert_rpc_bg "runSnapshotJobBg" "Bcachefs" "runSnapshotJobBg" \
        "{\"uuid\":\"$SNAP_JOB_UUID\"}"

    SNAP_DIR="${MNT}/.snapshots"
    if ls "$SNAP_DIR"/*/test-* &>/dev/null 2>&1; then
        _pass "runSnapshotJobBg — snapshot created in $SNAP_DIR"
    else
        _fail "runSnapshotJobBg — no snapshot found under $SNAP_DIR"
    fi

    assert_rpc_bg "runSnapshotJobBg (second run)" "Bcachefs" "runSnapshotJobBg" \
        "{\"uuid\":\"$SNAP_JOB_UUID\"}"

    SNAP_COUNT=$(find "$SNAP_DIR" -maxdepth 2 -name 'test-*' 2>/dev/null | wc -l)
    info "Snapshot count after 2 runs (retention=5): $SNAP_COUNT"
    if [ "$SNAP_COUNT" -le 5 ]; then
        _pass "runSnapshotJobBg — count within retention limit"
    else
        _fail "runSnapshotJobBg — count $SNAP_COUNT exceeds retention 5"
    fi

    # Snapshots are listed as snapshots (checks the JSON normaliser).
    SNAP_ERR=$(mktemp)
    FIRST_SNAP=$(rpc "Bcachefs" "getSubvolumeList" "{}" 2>/dev/null | python3 -c "
import sys, json
rows = [r for r in json.load(sys.stdin) if r.get('filesystem') == '$FS_UUID'
        and '/test-' in r.get('relpath', '')]
assert rows, 'no snapshot rows listed'
assert all(r['issnapshot'] for r in rows), 'snapshot not flagged: %r' % rows
assert all(r['readonly'] for r in rows), 'read-only snapshot not flagged: %r' % rows
print(sorted(r['path'] for r in rows)[0])
" 2>"$SNAP_ERR")
    if [ -n "$FIRST_SNAP" ]; then
        _pass "getSubvolumeList — snapshots flagged as read-only snapshots"
    else
        _fail "getSubvolumeList — snapshot parsing" "$(tail -1 "$SNAP_ERR")"
    fi
    rm -f "$SNAP_ERR"

    # Snapshots can now be deleted from the Subvolumes page.
    if [ -n "$FIRST_SNAP" ]; then
        assert_rpc "deleteSubvolume — snapshot" "Bcachefs" "deleteSubvolume" \
            "{\"path\":\"$FIRST_SNAP\"}" >/dev/null
        if [ ! -e "$FIRST_SNAP" ]; then
            _pass "deleteSubvolume — snapshot removed"
        else
            _fail "deleteSubvolume — snapshot still exists: $FIRST_SNAP"
        fi
    fi

    # Pruning no longer falls back to rm -rf: a plain directory that merely
    # looks like an old snapshot must survive, and the job must report it.
    DECOY="$SNAP_DIR/root/test-20000101-000000"
    mkdir -p "$DECOY" && echo keep > "$DECOY/file"
    if /usr/sbin/omv-bcachefs-snapshot "$MNT" test 1 &>/dev/null; then
        _fail "snapshot prune — job succeeded although a prune failed"
    else
        _pass "snapshot prune — job reports the failed prune"
    fi
    if [ -f "$DECOY/file" ]; then
        _pass "snapshot prune — non-snapshot directory left intact"
    else
        _fail "snapshot prune — non-snapshot directory was deleted"
    fi
    REAL_SNAPS=$(find "$SNAP_DIR/root" -maxdepth 1 -name 'test-2*' ! -name 'test-2000*' | wc -l)
    assert_eq "snapshot prune — real snapshots pruned to retention" "1" "$REAL_SNAPS"
    rm -rf "$DECOY"
fi

# ===========================================================================
section "Snapshot jobs — delete"
# ===========================================================================

if [ -n "$SNAP_JOB_UUID" ]; then
    assert_rpc "deleteSnapshotJob" "Bcachefs" "deleteSnapshotJob" \
        "{\"uuid\":\"$SNAP_JOB_UUID\"}" >/dev/null
    SNAP_JOB_UUID=""
    assert_rpc "getSnapshotJobList (empty)" "Bcachefs" \
        "getSnapshotJobList" "$LIST_PARAMS" >/dev/null
fi

# ===========================================================================
section "Scrub jobs — CRUD"
# ===========================================================================

SET_SCRUB_PARAMS=$(python3 -c "
import json
print(json.dumps({
    'uuid':             '$OMV_NEW_UUID',
    'enable':           True,
    'fsuuid':           '$FS_UUID',
    'execution':        'weekly',
    'minute':           ['0'],
    'hour':             ['3'],
    'dayofmonth':       ['*'],
    'month':            ['*'],
    'dayofweek':        ['0'],
    'everynminute':     False,
    'everynhour':       False,
    'everyndayofmonth': False,
    'sendemail':        False,
    'emailonerror':     False,
    'comment':          'rpc scrub test',
}))
")

SCRUB_RESULT=$(rpc "Bcachefs" "setScrubJob" "$SET_SCRUB_PARAMS" 2>&1)
SCRUB_JOB_UUID=$(echo "$SCRUB_RESULT" | python3 -c \
    "import sys,json; print(json.load(sys.stdin).get('uuid',''))" 2>/dev/null || echo "")

if [ -n "$SCRUB_JOB_UUID" ] && [ "$SCRUB_JOB_UUID" != "$OMV_NEW_UUID" ]; then
    _pass "setScrubJob (create) — UUID: $SCRUB_JOB_UUID"
else
    _fail "setScrubJob (create)" "No UUID in: ${SCRUB_RESULT:0:200}"
fi

assert_rpc "getScrubJobList" "Bcachefs" "getScrubJobList" \
    "$LIST_PARAMS" "rpc scrub test" >/dev/null

if [ -n "$SCRUB_JOB_UUID" ]; then
    assert_rpc "getScrubJob" "Bcachefs" "getScrubJob" \
        "{\"uuid\":\"$SCRUB_JOB_UUID\"}" "rpc scrub test" >/dev/null
fi

# ===========================================================================
section "Scrub jobs — run"
# ===========================================================================

if [ -n "$SCRUB_JOB_UUID" ]; then
    SCRUB_LOG=/var/log/omv-bcachefs-scrub.log
    SCRUB_LOG_LINES=$(wc -l < "$SCRUB_LOG" 2>/dev/null || echo 0)
    SCRUB_OUT=$(assert_rpc "runScrubJob" "Bcachefs" "runScrubJob" \
        "{\"uuid\":\"$SCRUB_JOB_UUID\"}")

    # Regression: the scrub used to target /dev/disk/by-uuid/<uuid>, a single
    # member, so the other members of a multi-device filesystem were never
    # scrubbed. It must scrub through the mount point.
    if tail -n +"$((SCRUB_LOG_LINES + 1))" "$SCRUB_LOG" 2>/dev/null \
            | grep -qF "Starting bcachefs scrub on $MNT (UUID: $FS_UUID)"; then
        _pass "runScrubJob — scrubs the whole filesystem through its mount point"
    else
        _fail "runScrubJob — scrub did not run on $MNT" \
            "$(tail -n 3 "$SCRUB_LOG" 2>/dev/null)"
    fi

    if /usr/sbin/bcachefs damage --help &>/dev/null; then
        if echo "$SCRUB_OUT" | grep -q "No damaged files recorded"; then
            _pass "runScrubJob — reports no damaged files"
        else
            _fail "runScrubJob — damage report missing" "${SCRUB_OUT:0:200}"
        fi
    fi

    # A filesystem that is not mounted cannot be scrubbed: fail clearly.
    ORPHAN_PARAMS=$(echo "$SET_SCRUB_PARAMS" | python3 -c "
import sys, json
p = json.load(sys.stdin)
p['fsuuid'] = '00000000-0000-0000-0000-000000000000'
p['enable'] = False
p['comment'] = 'rpc scrub unmounted'
print(json.dumps(p))")
    ORPHAN_JOB=$(rpc "Bcachefs" "setScrubJob" "$ORPHAN_PARAMS" 2>/dev/null \
        | python3 -c "import sys,json; print(json.load(sys.stdin).get('uuid',''))" 2>/dev/null)
    if [ -n "$ORPHAN_JOB" ]; then
        assert_rpc_fails "runScrubJob — unmounted filesystem fails" "Bcachefs" \
            "runScrubJob" "{\"uuid\":\"$ORPHAN_JOB\"}"
        rpc "Bcachefs" "deleteScrubJob" "{\"uuid\":\"$ORPHAN_JOB\"}" &>/dev/null || true
    else
        _fail "setScrubJob — could not create the unmounted-filesystem job"
    fi
fi

# ===========================================================================
section "Scrub jobs — cron file"
# ===========================================================================

# A % in a job comment used to end the cron command (cron turns it into a
# newline), and the comment was not shell-quoted, so $(...) in it ran as
# root. Render the cron file and check the job line is safe.
if [ -n "$SCRUB_JOB_UUID" ]; then
    CRON_PARAMS=$(echo "$SET_SCRUB_PARAMS" | python3 -c "
import sys, json
p = json.load(sys.stdin)
p['uuid'] = '$SCRUB_JOB_UUID'
p['sendemail'] = True
p['comment'] = \"rpc scrub 50% 'done' \$(true)\"
print(json.dumps(p))")
    assert_rpc "setScrubJob — comment with %, quotes and \$()" "Bcachefs" \
        "setScrubJob" "$CRON_PARAMS" >/dev/null
    if omv-salt deploy run --quiet bcachesfcron &>/dev/null; then
        CRON_LINE=$(grep -F "omv-bcachefs-scrub '$FS_UUID'" \
            /etc/cron.d/openmediavault-bcachefs-scrub)
        EXPECTED="-c 'rpc scrub 50\\% '\\''done'\\'' \$(true)'"
        if [[ "$CRON_LINE" == *"$EXPECTED"* ]]; then
            _pass "cron — comment single-quoted and % escaped"
        else
            _fail "cron — unexpected job line" "$CRON_LINE"
        fi
    else
        _fail "cron — omv-salt deploy run bcachesfcron failed"
    fi
fi

# ===========================================================================
section "Scrub jobs — delete"
# ===========================================================================

if [ -n "$SCRUB_JOB_UUID" ]; then
    assert_rpc "deleteScrubJob" "Bcachefs" "deleteScrubJob" \
        "{\"uuid\":\"$SCRUB_JOB_UUID\"}" >/dev/null
    SCRUB_JOB_UUID=""
    assert_rpc "getScrubJobList (empty)" "Bcachefs" \
        "getScrubJobList" "$LIST_PARAMS" >/dev/null
fi

# ===========================================================================
section "Auto-unlock — enable and disable"
# ===========================================================================

KEYS_DIR="/etc/bcachefs/keys"

assert_rpc "enableAutoUnlock" "Bcachefs" "enableAutoUnlock" \
    "{\"uuid\":\"$FS_UUID\",\"passphrase\":\"testpassphrase123\"}" >/dev/null

KEY_FILE="${KEYS_DIR}/${FS_UUID}.key"
if [ -f "$KEY_FILE" ]; then
    _pass "enableAutoUnlock — key file created: $KEY_FILE"
else
    _fail "enableAutoUnlock — key file not found: $KEY_FILE"
fi

if [ -f "$KEY_FILE" ]; then
    KEY_PERMS=$(stat -c "%a" "$KEY_FILE")
    if [ "$KEY_PERMS" = "600" ]; then
        _pass "enableAutoUnlock — key file permissions are 600"
    else
        _fail "enableAutoUnlock — key file permissions are $KEY_PERMS, expected 600"
    fi
fi

assert_rpc "getFilesystem — autounlock=true" "Bcachefs" "getFilesystem" \
    "{\"uuid\":\"$FS_UUID\"}" '"autounlock":true' >/dev/null

assert_rpc "disableAutoUnlock" "Bcachefs" "disableAutoUnlock" \
    "{\"uuid\":\"$FS_UUID\"}" >/dev/null

if [ ! -f "$KEY_FILE" ]; then
    _pass "disableAutoUnlock — key file removed"
else
    _fail "disableAutoUnlock — key file still exists"
fi

assert_rpc "getFilesystem — autounlock=false" "Bcachefs" "getFilesystem" \
    "{\"uuid\":\"$FS_UUID\"}" '"autounlock":false' >/dev/null

# ===========================================================================
section "Subvolumes — delete"
# ===========================================================================

assert_rpc "deleteSubvolume" "Bcachefs" "deleteSubvolume" \
    "{\"path\":\"$SUBVOL_PATH\"}" >/dev/null

if [ ! -d "$SUBVOL_PATH" ]; then
    _pass "deleteSubvolume — directory removed"
else
    _fail "deleteSubvolume — directory still exists"
fi

assert_rpc "getSubvolumeList (root only)" "Bcachefs" \
    "getSubvolumeList" "{}" "root" >/dev/null

assert_rpc_fails "deleteSubvolume — non-existent path" "Bcachefs" \
    "deleteSubvolume" "{\"path\":\"$SUBVOL_PATH\"}"

# ===========================================================================
section "Validation — negative tests"
# ===========================================================================

assert_rpc_fails "setSnapshotJob — retention=0" "Bcachefs" "setSnapshotJob" \
    "$(python3 -c "
import json
print(json.dumps({
    'uuid': '$OMV_NEW_UUID', 'enable': True, 'subvolume': '$MNT',
    'prefix': 'x', 'retention': 0, 'retentionunit': 'count',
    'readonly': False, 'execution': 'daily',
    'minute': ['0'], 'hour': ['0'], 'dayofmonth': ['*'],
    'month': ['*'], 'dayofweek': ['*'],
    'everynminute': False, 'everynhour': False, 'everyndayofmonth': False,
    'sendemail': False, 'emailonerror': False, 'comment': '',
}))
")"

assert_rpc_fails "setScrubJob — missing uuid" "Bcachefs" "setScrubJob" \
    '{"enable":true,"fsuuid":"fake","execution":"daily"}'

assert_rpc_fails "enableAutoUnlock — missing passphrase" "Bcachefs" \
    "enableAutoUnlock" "{\"uuid\":\"$FS_UUID\"}"

assert_rpc_fails "getFilesystem — unknown UUID" "Bcachefs" "getFilesystem" \
    '{"uuid":"00000000-0000-0000-0000-000000000000"}'

assert_rpc_fails "createFilesystem — encrypted without passphrase" "Bcachefs" \
    "createFilesystem" "$(python3 -c "
import json
print(json.dumps({
    'label':       '',
    'replicas':    1,
    'compression': 'none',
    'checksum':    'crc32c',
    'nocow':       False,
    'encrypted':   True,
    'passphrase':  '',
    'autounlock':  False,
    'devicefiles': '${DEVICES[0]}',
}))")"

assert_rpc_fails "createFilesystem — string booleans, encrypted without passphrase" \
    "Bcachefs" "createFilesystem" "$(python3 -c "
import json
print(json.dumps({
    'label':       '',
    'replicas':    '1',
    'compression': 'none',
    'checksum':    'crc32c',
    'nocow':       'false',
    'encrypted':   'true',
    'passphrase':  '',
    'autounlock':  'false',
    'devicefiles': '${DEVICES[0]}',
}))")"

assert_rpc_fails "createFilesystem — device in multiple groups" \
    "Bcachefs" "createFilesystem" "$(python3 -c "
import json
print(json.dumps({
    'label':             '',
    'replicas':          1,
    'enablegroups':      True,
    'group1name':        'ssd',
    'group1devices':     '${DEVICES[0]}',
    'group2name':        'hdd',
    'group2devices':     '${DEVICES[0]}',
    'group3name':        '',
    'group3devices':     '',
    'foreground_target': '',
    'promote_target':    '',
    'background_target': '',
    'compression':       'none',
    'checksum':          'crc32c',
    'nocow':             False,
    'encrypted':         False,
    'passphrase':        '',
    'autounlock':        False,
    'devicefiles':       '',
}))")"

assert_rpc_fails "createFilesystem — enablegroups with no groups configured" \
    "Bcachefs" "createFilesystem" "$(python3 -c "
import json
print(json.dumps({
    'label':             '',
    'replicas':          1,
    'enablegroups':      True,
    'group1name':        '',
    'group1devices':     '',
    'group2name':        '',
    'group2devices':     '',
    'group3name':        '',
    'group3devices':     '',
    'foreground_target': '',
    'promote_target':    '',
    'background_target': '',
    'compression':       'none',
    'checksum':          'crc32c',
    'nocow':             False,
    'encrypted':         False,
    'passphrase':        '',
    'autounlock':        False,
    'devicefiles':       '',
}))")"

# Values that end up on the format command line are now validated before
# any device is wiped. Each of these must be rejected up front.
create_params_with() {  # key value [key value ...] -> createFilesystem JSON
    python3 - "$@" <<EOF
import json, sys
p = {
    'label': '', 'replicas': 1, 'enablegroups': True,
    'group1name': 'ssd', 'group1devices': '${DEVICES[0]}',
    'group2name': '', 'group2devices': '', 'group3name': '', 'group3devices': '',
    'foreground_target': '', 'promote_target': '', 'background_target': '',
    'compression': 'none', 'checksum': 'crc32c', 'nocow': False,
    'encrypted': False, 'passphrase': '', 'autounlock': False, 'devicefiles': '',
}
args = sys.argv[1:]
for k, v in zip(args[::2], args[1::2]):
    p[k] = v
print(json.dumps(p))
EOF
}

assert_rpc_fails "createFilesystem — group name with a space" "Bcachefs" \
    "createFilesystem" "$(create_params_with group1name 'ss d')"
assert_rpc_fails "createFilesystem — group name with a dot" "Bcachefs" \
    "createFilesystem" "$(create_params_with group1name 'ssd.fast')"
assert_rpc_fails "createFilesystem — target with shell characters" "Bcachefs" \
    "createFilesystem" "$(create_params_with foreground_target 'ssd;reboot')"
assert_rpc_fails "createFilesystem — failure domain with a space" "Bcachefs" \
    "createFilesystem" "$(create_params_with group1failuredomain 'shelf 1')"
assert_rpc_fails "createFilesystem — label longer than 32 bytes" "Bcachefs" \
    "createFilesystem" "$(create_params_with enablegroups false devicefiles "${DEVICES[0]}" \
        label 'this label is much too long for bcachefs')"
assert_rpc_fails "createFilesystem — negative missing_dev_timeout" "Bcachefs" \
    "createFilesystem" "$(create_params_with missing_dev_timeout -5)"
if /usr/sbin/bcachefs show-super "${DEVICES[0]}" 2>/dev/null | grep -q "^External UUID:.*$FS_UUID"; then
    _pass "createFilesystem — rejected requests left ${DEVICES[0]} untouched"
else
    _fail "createFilesystem — ${DEVICES[0]} was modified by a rejected request"
fi

assert_rpc_fails "setDeviceFailureDomain — invalid name" "Bcachefs" \
    "setDeviceFailureDomain" \
    "{\"uuid\":\"$FS_UUID\",\"devicefile\":\"${DEVICES[0]}\",\"failure_domain\":\"a b\"}"
assert_rpc_fails "setDeviceFailureDomain — device not in the filesystem" "Bcachefs" \
    "setDeviceFailureDomain" \
    "{\"uuid\":\"$FS_UUID\",\"devicefile\":\"/dev/null\",\"failure_domain\":\"x\"}"
assert_rpc_fails "setDeviceFailureDomain — unknown filesystem" "Bcachefs" \
    "setDeviceFailureDomain" \
    "{\"uuid\":\"00000000-0000-0000-0000-000000000000\",\"devicefile\":\"${DEVICES[0]}\",\"failure_domain\":\"x\"}"
assert_rpc_fails "addDevice — failure domain with shell characters" "Bcachefs" \
    "addDevice" "{\"uuid\":\"$FS_UUID\",\"devicefile\":\"${DEVICES[0]}\",\"failure_domain\":\"\$(reboot)\"}"
assert_rpc_fails "removeDevice — negative slot" "Bcachefs" "removeDevice" \
    "{\"devicefile\":\"\",\"uuid\":\"$FS_UUID\",\"slot\":-1}"

# ===========================================================================
section "Devices — set-state, evacuate, remove, add"
# ===========================================================================

# These tests mutate filesystem membership, so they run on the single-device
# filesystem (which spans all supplied devices) before the tiering test
# reformats everything. They require a spare member to remove/re-add.
if [ ${#DEVICES[@]} -lt 2 ]; then
    info "Skipping device add/remove/state tests — requires >=2 devices (got ${#DEVICES[@]})"
else
    LAST_DEV="${DEVICES[$((${#DEVICES[@]} - 1))]}"
    info "Operating on member device: $LAST_DEV"

    # Reversible state changes (do not alter membership).
    assert_rpc "setDeviceState — ro" "Bcachefs" "setDeviceState" \
        "{\"devicefile\":\"$LAST_DEV\",\"state\":\"ro\"}" >/dev/null
    assert_rpc "setDeviceState — rw" "Bcachefs" "setDeviceState" \
        "{\"devicefile\":\"$LAST_DEV\",\"state\":\"rw\"}" >/dev/null

    # Move data off, remove from the filesystem, then add it back.
    assert_rpc_bg "evacuateDevice" "Bcachefs" "evacuateDevice" \
        "{\"devicefile\":\"$LAST_DEV\"}"
    assert_rpc_bg "removeDevice" "Bcachefs" "removeDevice" \
        "{\"devicefile\":\"$LAST_DEV\"}"
    assert_rpc_bg "addDevice (re-add, with failure domain)" "Bcachefs" "addDevice" \
        "{\"uuid\":\"$FS_UUID\",\"devicefile\":\"$LAST_DEV\",\"failure_domain\":\"shelfB\"}"
    assert_eq "addDevice — failure domain applied" "shelfB" \
        "$(dev_field "$LAST_DEV" failure_domain)"

    # Failure domains on existing members: put every device in one domain,
    # check the report groups them, then make each its own domain again.
    for dev in "${DEVICES[@]}"; do
        assert_rpc "setDeviceFailureDomain — shelfA on $dev" "Bcachefs" \
            "setDeviceFailureDomain" \
            "{\"uuid\":\"$FS_UUID\",\"devicefile\":\"$dev\",\"failure_domain\":\"shelfA\"}" >/dev/null
    done
    assert_eq "getDeviceList — failure domain shelfA" "shelfA" \
        "$(dev_field "${DEVICES[0]}" failure_domain)"
    if /usr/sbin/bcachefs fs failure-domains --help &>/dev/null; then
        FD_ERR=$(mktemp)
        if rpc "Bcachefs" "getFailureDomains" "{\"uuid\":\"$FS_UUID\"}" 2>&1 | python3 -c "
import sys, json
d = json.load(sys.stdin)
names = [x['domain'] for x in d['domains']]
assert names == ['shelfA'], 'expected only shelfA: %r' % d
assert d['domains'][0]['devices'] == ${#DEVICES[@]}, d
" 2>"$FD_ERR"; then
            _pass "getFailureDomains — all devices in shelfA"
        else
            _fail "getFailureDomains — shelfA not reported" "$(tail -1 "$FD_ERR")"
        fi
        rm -f "$FD_ERR"
    fi
    for dev in "${DEVICES[@]}"; do
        rpc "Bcachefs" "setDeviceFailureDomain" \
            "{\"uuid\":\"$FS_UUID\",\"devicefile\":\"$dev\",\"failure_domain\":\"\"}" &>/dev/null
    done
    assert_eq "setDeviceFailureDomain — empty value clears it" "" \
        "$(dev_field "${DEVICES[0]}" failure_domain)"

    # Offline / online. The just re-added member holds no data, so taking it
    # offline cannot make any data unavailable.
    OFF_SLOT=$(dev_field "$LAST_DEV" slot)
    assert_rpc "offlineDevice" "Bcachefs" "offlineDevice" \
        "{\"devicefile\":\"$LAST_DEV\",\"force\":true}" >/dev/null
    assert_eq "getDeviceList — offline member still listed as offline" "offline" \
        "$(slot_field "$OFF_SLOT" state)"
    assert_eq "getDeviceList — offline member not online" "false" \
        "$(slot_field "$OFF_SLOT" online)"
    OFF_DEV=$(slot_field "$OFF_SLOT" devicefile)
    if [ -n "$OFF_DEV" ] && [ "$(realpath "$OFF_DEV" 2>/dev/null)" = "$(realpath "$LAST_DEV")" ]; then
        _pass "getDeviceList — offline member named by its last device ($OFF_DEV)"
    else
        _fail "getDeviceList — offline member device name" "got '$OFF_DEV', expected $LAST_DEV"
    fi
    assert_rpc "onlineDevice" "Bcachefs" "onlineDevice" \
        "{\"devicefile\":\"$LAST_DEV\"}" >/dev/null
    assert_eq "getDeviceList — member back online" "true" "$(dev_field "$LAST_DEV" online)"
    assert_eq "getDeviceList — member back to rw" "rw" "$(dev_field "$LAST_DEV" state)"

    # An offline member has no device node, so it is removed by slot.
    assert_rpc "offlineDevice (for removal)" "Bcachefs" "offlineDevice" \
        "{\"devicefile\":\"$LAST_DEV\",\"force\":true}" >/dev/null
    assert_rpc_bg "removeDevice — offline member by slot" "Bcachefs" "removeDevice" \
        "{\"devicefile\":\"\",\"uuid\":\"$FS_UUID\",\"slot\":\"$OFF_SLOT\"}"
    assert_eq "removeDevice — offline member gone" "" "$(slot_field "$OFF_SLOT" state)"
    wipefs -a "$LAST_DEV" &>/dev/null || true
    assert_rpc_bg "addDevice (re-add after offline removal)" "Bcachefs" "addDevice" \
        "{\"uuid\":\"$FS_UUID\",\"devicefile\":\"$LAST_DEV\"}"

    # The UI's "Set failed" button sends state=failed; newer bcachefs-tools
    # renamed that device state to "evacuating" (valid values: rw, ro,
    # evacuating, spare). The RPC normalises either name to whatever the
    # installed tool accepts, so both must succeed. Run on the just re-added
    # member so it holds no data and --force can take it out of service.
    assert_rpc "setDeviceState — failed" "Bcachefs" "setDeviceState" \
        "{\"devicefile\":\"$LAST_DEV\",\"state\":\"failed\"}" >/dev/null
    assert_rpc "setDeviceState — rw (restore after failed)" "Bcachefs" "setDeviceState" \
        "{\"devicefile\":\"$LAST_DEV\",\"state\":\"rw\"}" >/dev/null
    assert_rpc "setDeviceState — evacuating" "Bcachefs" "setDeviceState" \
        "{\"devicefile\":\"$LAST_DEV\",\"state\":\"evacuating\"}" >/dev/null
    assert_rpc "setDeviceState — rw (restore after evacuating)" "Bcachefs" "setDeviceState" \
        "{\"devicefile\":\"$LAST_DEV\",\"state\":\"rw\"}" >/dev/null

    assert_rpc_fails "setDeviceState — invalid state" "Bcachefs" "setDeviceState" \
        "{\"devicefile\":\"$LAST_DEV\",\"state\":\"bogus\"}"

    # Regression test: removing the *first* member (rather than the last)
    # leaves a gap in bcachefs's member-slot numbering -- the remaining
    # device(s) keep their original, higher slot numbers instead of sliding
    # down to 0..N-1. getDeviceList() used to build its device list by
    # reindexing sysfs's dev-N entries into a fresh 0..N-1 array and then
    # matching against `bcachefs show-super`'s "Device N:" blocks by that
    # recomputed position, so a gap made it read the wrong member's (or a
    # nonexistent member's) state for a device -- surfacing as an incorrect
    # RO/failed badge on a device the user never touched, or state "unknown".
    FIRST_DEV="${DEVICES[0]}"
    CHECK_DEV="$LAST_DEV"   # distinct from FIRST_DEV whenever count >= 2
    info "Regression test: removing $FIRST_DEV to open a gap in member-slot numbering"

    assert_rpc_bg "evacuateDevice (gap regression)" "Bcachefs" "evacuateDevice" \
        "{\"devicefile\":\"$FIRST_DEV\"}"
    assert_rpc_bg "removeDevice (gap regression)" "Bcachefs" "removeDevice" \
        "{\"devicefile\":\"$FIRST_DEV\"}"

    # Force the surviving device to a known, non-default state so a
    # misaligned lookup (wrong device, or a missing/"unknown" entry) is
    # unambiguous rather than accidentally matching the default state.
    assert_rpc "setDeviceState — ro (gap regression)" "Bcachefs" "setDeviceState" \
        "{\"devicefile\":\"$CHECK_DEV\",\"state\":\"ro\"}" >/dev/null

    GAP_DEV_LIST=$(rpc "Bcachefs" "getDeviceList" "{}" 2>/dev/null || echo "[]")
    GAP_ERR=$(mktemp)
    # getDeviceList reports device-mapper members as /dev/mapper/<name> (what
    # the UI shows), while $DEVICES holds the readlink -f canonical form
    # (e.g. /dev/dm-N) set up at the top of this script. Resolve both sides
    # with realpath before comparing so this works for plain disks and dm
    # devices alike.
    if echo "$GAP_DEV_LIST" | python3 -c "
import sys, json, os

def real(p):
    try:
        return os.path.realpath(p)
    except OSError:
        return p

rows = [r for r in json.load(sys.stdin) if r.get('filesystem') == '$FS_UUID']
for r in rows:
    r['_real'] = real(r.get('devicefile', ''))

assert len(rows) == ${#DEVICES[@]} - 1, \
    'expected ${#DEVICES[@]} - 1 device(s) after removal, got %d: %r' % (len(rows), rows)
assert not any(r['_real'] == '$FIRST_DEV' for r in rows), \
    'removed device $FIRST_DEV still listed: %r' % rows
assert not any(r.get('state') == 'unknown' for r in rows), \
    'a device reported state unknown after the slot gap: %r' % rows
check = next((r for r in rows if r['_real'] == '$CHECK_DEV'), None)
assert check is not None, '$CHECK_DEV row missing from getDeviceList (rows: %r)' % rows
assert check.get('state') == 'ro', \
    'expected $CHECK_DEV state ro, got %r' % check.get('state')
" 2>"$GAP_ERR"; then
        _pass "getDeviceList — correct device/state mapping after member-slot gap"
    else
        _fail "getDeviceList — device/state misaligned after member-slot gap" \
            "$(cat "$GAP_ERR" 2>/dev/null)"
    fi
    rm -f "$GAP_ERR"

    # Restore state and membership so later sections see the expected device
    # count and every device back in the rw state.
    rpc "Bcachefs" "setDeviceState" \
        "{\"devicefile\":\"$CHECK_DEV\",\"state\":\"rw\"}" &>/dev/null || true
    assert_rpc_bg "addDevice (gap regression re-add)" "Bcachefs" "addDevice" \
        "{\"uuid\":\"$FS_UUID\",\"devicefile\":\"$FIRST_DEV\"}"
fi

# ===========================================================================
section "Filesystem — create with device label groups (tiering)"
# ===========================================================================

if [ ${#DEVICES[@]} -lt 2 ]; then
    info "Skipping tiering test — requires at least 2 devices (got ${#DEVICES[@]})"
else
    # Tear down the previous filesystem so we can reformat the same devices.
    if [ -n "$MNT" ] && mountpoint -q "$MNT" 2>/dev/null; then
        info "Unmounting $MNT for tiering test"
        umount -l "$MNT" 2>/dev/null || true
    fi
    wipefs -a "${DEVICES[@]}" 2>/dev/null || true

    # Group 1 (ssd): first device. Group 2 (hdd): remaining devices.
    GROUP1_DEVS="${DEVICES[0]}"
    GROUP2_DEVS=$(IFS=,; echo "${DEVICES[*]:1}")

    TIERING_PARAMS=$(python3 -c "
import json
print(json.dumps({
    'label':             'omvbcachefstier',
    'replicas':          1,
    'enablegroups':      True,
    'group1name':        'ssd',
    'group1devices':     '$GROUP1_DEVS',
    'group1failuredomain': 'shelfA',
    'group2name':        'hdd',
    'group2devices':     '$GROUP2_DEVS',
    'group3name':        '',
    'group3devices':     '',
    'foreground_target': 'ssd',
    'promote_target':    'ssd',
    'background_target': 'hdd',
    'compression':       'none',
    'checksum':          'crc32c',
    'erasurecode':       False,
    'nocow':             False,
    'encrypted':         False,
    'passphrase':        '',
    'autounlock':        False,
    'devicefiles':       '',
}))
")

    assert_rpc_bg "createFilesystem (tiering)" "Bcachefs" "createFilesystem" "$TIERING_PARAMS"

    info "Reading superblock from ${DEVICES[0]} ..."
    TIER_SUPER=$(/usr/sbin/bcachefs show-super "${DEVICES[0]}" 2>/dev/null || true)
    FS_UUID=$(echo "$TIER_SUPER" | awk '/^External UUID:/ {print $NF}')

    if [ -n "$FS_UUID" ]; then
        _pass "createFilesystem (tiering) — UUID detected: $FS_UUID"
    else
        _fail "createFilesystem (tiering) — could not read UUID from superblock"
    fi

    TIER_LABEL=$(echo "$TIER_SUPER" | awk '/^Label:/ {print $NF}')
    if [ "$TIER_LABEL" = "omvbcachefstier" ]; then
        _pass "createFilesystem (tiering) — label 'omvbcachefstier' in superblock"
    else
        _fail "createFilesystem (tiering) — expected label 'omvbcachefstier', got '$TIER_LABEL'"
    fi

    if /usr/sbin/bcachefs show-super "${DEVICES[0]}" 2>/dev/null | grep -q "ssd\.ssd1"; then
        _pass "tiering — label ssd.ssd1 found on ${DEVICES[0]}"
    else
        _fail "tiering — label ssd.ssd1 not found on ${DEVICES[0]}"
    fi

    if /usr/sbin/bcachefs show-super "${DEVICES[1]}" 2>/dev/null | grep -q "hdd\.hdd1"; then
        _pass "tiering — label hdd.hdd1 found on ${DEVICES[1]}"
    else
        _fail "tiering — label hdd.hdd1 not found on ${DEVICES[1]}"
    fi

    # Group 1 has a failure domain, group 2 does not. format carries device
    # options over to later devices, so group 2 must have been reset.
    TIER_DOMAINS=$(echo "$TIER_SUPER" | awk '
        /^Device [0-9]+:/      { dev = $2 }
        /^  Failure domain:/   { print dev $3 }')
    assert_eq "tiering — only group 1 is in failure domain shelfA" "0:shelfA" \
        "$TIER_DOMAINS"

    # Mount the tiering filesystem (bcachefs accepts colon-separated devices).
    MNT="/srv/dev-disk-by-uuid-${FS_UUID}"
    mkdir -p "$MNT"
    MOUNT_DEVS=$(IFS=:; echo "${DEVICES[*]}")
    if mount -t bcachefs "$MOUNT_DEVS" "$MNT" 2>/dev/null; then
        _pass "mount tiering filesystem at $MNT"
    else
        _fail "mount tiering filesystem at $MNT"
    fi
fi

# ===========================================================================
section "Filesystem — create with erasure coding"
# ===========================================================================

# Erasure coding stripes data across several devices, so it needs at least 3.
if [ ${#DEVICES[@]} -lt 3 ]; then
    info "Skipping erasure coding test — requires >=3 devices (got ${#DEVICES[@]})"
else
    # Tear down the previous (tiering) filesystem so we can reformat.
    if [ -n "$MNT" ] && mountpoint -q "$MNT" 2>/dev/null; then
        info "Unmounting $MNT for erasure coding test"
        umount -l "$MNT" 2>/dev/null || true
    fi
    wipefs -a "${DEVICES[@]}" 2>/dev/null || true

    EC_PARAMS=$(python3 -c "
import json
print(json.dumps({
    'label':       'omvbcachefsec',
    'replicas':    1,
    'compression': 'none',
    'checksum':    'crc32c',
    'erasurecode': True,
    'nocow':       False,
    'encrypted':   False,
    'passphrase':  '',
    'autounlock':  False,
    'devicefiles': '$DEVICEFILES_CSV',
}))
")

    assert_rpc_bg "createFilesystem (erasure coding)" "Bcachefs" \
        "createFilesystem" "$EC_PARAMS"

    info "Reading superblock from ${DEVICES[0]} ..."
    EC_SUPER=$(/usr/sbin/bcachefs show-super "${DEVICES[0]}" 2>/dev/null || true)
    FS_UUID=$(echo "$EC_SUPER" | awk '/^External UUID:/ {print $NF}')

    if [ -n "$FS_UUID" ]; then
        _pass "createFilesystem (erasure coding) — UUID detected: $FS_UUID"
    else
        _fail "createFilesystem (erasure coding) — could not read UUID from superblock"
    fi

    if echo "$EC_SUPER" | grep -qi "erasure_code"; then
        _pass "createFilesystem (erasure coding) — erasure_code enabled in superblock"
    else
        _fail "createFilesystem (erasure coding) — erasure_code not found in superblock"
    fi

    # Mount so cleanup unmounts/wipes this filesystem too.
    if [ -n "$FS_UUID" ]; then
        MNT="/srv/dev-disk-by-uuid-${FS_UUID}"
        mkdir -p "$MNT"
        MOUNT_DEVS=$(IFS=:; echo "${DEVICES[*]}")
        if mount -t bcachefs "$MOUNT_DEVS" "$MNT" 2>/dev/null; then
            _pass "mount erasure coding filesystem at $MNT"
        else
            _fail "mount erasure coding filesystem at $MNT"
        fi
    fi
fi

# ===========================================================================
section "Filesystem — label quoting and missing-device timeout"
# ===========================================================================

# The label used to go onto the format command line unquoted: a space broke
# the format after the devices had already been wiped, and shell syntax ran.
if [ -n "$MNT" ] && mountpoint -q "$MNT" 2>/dev/null; then
    umount -l "$MNT" 2>/dev/null || true
fi
wipefs -a "${DEVICES[@]}" 2>/dev/null || true

PWN_FILE=/tmp/omvbcfs-x
rm -f "$PWN_FILE"
QUOTE_LABEL='a "b" $(touch /tmp/omvbcfs-x)'   # 29 bytes
QUOTE_PARAMS=$(python3 - "$QUOTE_LABEL" <<EOF
import json, sys
print(json.dumps({
    'label': sys.argv[1], 'replicas': 1, 'compression': 'none',
    'checksum': 'crc32c', 'erasurecode': False, 'nocow': False,
    'encrypted': False, 'passphrase': '', 'autounlock': False,
    'missing_dev_timeout': 45, 'devicefiles': '${DEVICES[0]}',
}))
EOF
)
assert_rpc_bg "createFilesystem — label with spaces, quotes and \$()" "Bcachefs" \
    "createFilesystem" "$QUOTE_PARAMS"
QUOTE_SUPER=$(/usr/sbin/bcachefs show-super "${DEVICES[0]}" 2>/dev/null || true)
assert_eq "createFilesystem — label stored verbatim" "$QUOTE_LABEL" \
    "$(echo "$QUOTE_SUPER" | sed -n 's/^Label:[[:space:]]*//p')"
if [ ! -e "$PWN_FILE" ]; then
    _pass "createFilesystem — shell syntax in the label was not executed"
else
    _fail "createFilesystem — shell syntax in the label was executed"
    rm -f "$PWN_FILE"
fi
if echo "$QUOTE_SUPER" | grep -qE '^[[:space:]]*missing_dev_timeout:[[:space:]]+45$'; then
    _pass "createFilesystem — missing_dev_timeout=45 in superblock"
else
    _fail "createFilesystem — missing_dev_timeout not set"
fi

# ===========================================================================
section "Filesystem — encryption and auto-unlock"
# ===========================================================================

wipefs -a "${DEVICES[@]}" 2>/dev/null || true
ENC_PARAMS=$(python3 -c "
import json
print(json.dumps({
    'label': 'omvbcachefsenc', 'replicas': 1, 'compression': 'none',
    'checksum': 'crc32c', 'erasurecode': False, 'nocow': False,
    'encrypted': True, 'passphrase': 'test passphrase 123', 'autounlock': True,
    'devicefiles': '${DEVICES[0]}',
}))")
assert_rpc_bg "createFilesystem — encrypted with auto-unlock" "Bcachefs" \
    "createFilesystem" "$ENC_PARAMS"
# show-super prints nothing for an encrypted filesystem until its key is in
# the keyring, so take the UUID from blkid and the state from unlock -c.
FS_UUID=$(blkid -o value -s UUID "${DEVICES[0]}" 2>/dev/null)
if [ -n "$FS_UUID" ]; then
    _pass "createFilesystem (encrypted) — UUID detected: $FS_UUID"
else
    _fail "createFilesystem (encrypted) — could not read UUID with blkid"
fi
if /usr/sbin/bcachefs unlock -c "${DEVICES[0]}" </dev/null 2>&1 | grep -q "is encrypted"; then
    _pass "createFilesystem — filesystem is encrypted"
else
    _fail "createFilesystem — filesystem is not encrypted"
fi

KEY_FILE="/etc/bcachefs/keys/${FS_UUID}.key"
if [ -n "$FS_UUID" ] && [ -f "$KEY_FILE" ] && [ "$(stat -c %a "$KEY_FILE")" = "600" ]; then
    _pass "createFilesystem — auto-unlock key file written (mode 600)"
else
    _fail "createFilesystem — auto-unlock key file missing: $KEY_FILE"
fi

# Unlock the way the boot service does, then mount without a passphrase.
UNLOCK_OUT=$(/usr/sbin/omv-bcachefs-autounlock 2>&1)
if echo "$UNLOCK_OUT" | grep -q "unlocked $FS_UUID\|already unlocked"; then
    _pass "omv-bcachefs-autounlock — processed $FS_UUID"
else
    _fail "omv-bcachefs-autounlock — no result for $FS_UUID" "$UNLOCK_OUT"
fi
MNT="/srv/dev-disk-by-uuid-${FS_UUID}"
mkdir -p "$MNT"
if mount -t bcachefs "${DEVICES[0]}" "$MNT" </dev/null 2>/dev/null; then
    _pass "mount encrypted filesystem using the auto-unlock key"

    # Regression: encrypted used to read false for every filesystem.
    info "unlock -c: $(/usr/sbin/bcachefs unlock -c "${DEVICES[0]}" </dev/null 2>&1 | head -1)"
    assert_rpc "getFilesystem — encrypted=true" "Bcachefs" "getFilesystem" \
        "{\"uuid\":\"$FS_UUID\"}" '"encrypted":true' >/dev/null
    assert_rpc "getFilesystemList — encrypted=true, autounlock=true" "Bcachefs" \
        "getFilesystemList" "$LIST_PARAMS" '"encrypted":true,"autounlock":true' >/dev/null
    assert_rpc "getFilesystemList — label of encrypted filesystem" "Bcachefs" \
        "getFilesystemList" "$LIST_PARAMS" '"label":"omvbcachefsenc"' >/dev/null
    ENC_STATE=$(dev_field "${DEVICES[0]}" state)
    assert_eq "getDeviceList — encrypted filesystem member state" "rw" "$ENC_STATE"
    if [ "$ENC_STATE" != "rw" ]; then
        info "diagnostics for the failure above:"
        rpc "Bcachefs" "getDeviceList" "{}" 2>/dev/null | python3 -c "
import sys, json
for r in json.load(sys.stdin):
    if r.get('filesystem') == '$FS_UUID':
        print('    row:', json.dumps(r))" >&2
        for d in /sys/fs/bcachefs/*/dev-*; do
            echo "    $d: block -> $(readlink "$d/block" 2>&1);" \
                "state=[$(cat "$d/state" 2>&1)]" >&2
        done
        grep " /srv/dev-disk-by-uuid-$FS_UUID " /proc/mounts | sed 's/^/    mounts: /' >&2
        /usr/sbin/bcachefs show-super "${DEVICES[0]}" </dev/null 2>&1 \
            | grep -nE '^(External|Device [0-9])|State:' | sed 's/^/    show-super: /' >&2
    fi

    # show-super exits non-zero on an encrypted filesystem; the core
    # Filesystems "Details" view must still show the superblock.
    assert_rpc "FileSystemMgmt getDetails — encrypted filesystem" "FileSystemMgmt" \
        "getDetails" "{\"devicefile\":\"${DEVICES[0]}\"}" "$FS_UUID" >/dev/null
else
    _fail "mount encrypted filesystem using the auto-unlock key"
fi

# ===========================================================================
section "Summary"
# ===========================================================================
TOTAL=$((PASS + FAIL))
echo >&2
echo -e "  Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC} (${TOTAL} total)" >&2
if [ ${#FAILED_TESTS[@]} -gt 0 ]; then
    echo -e "\n  ${RED}Failed tests:${NC}" >&2
    for t in "${FAILED_TESTS[@]}"; do
        echo -e "    ${RED}✗${NC} $t" >&2
    done
fi
echo >&2

[ $FAIL -eq 0 ] && exit 0 || exit 1

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

DEVICES=("$@")

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
if mount -t bcachefs "${DEVICES[0]}" "$MNT" 2>/dev/null; then
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
" 2>/dev/null; then
    _pass "getSubvolumeList — test filesystem root present, inuse=true"
else
    _fail "getSubvolumeList — test filesystem root not found"
fi

assert_rpc "getSubvolumes (with subvol)" "Bcachefs" "getSubvolumes" \
    "{}" "$FS_UUID_SHORT" >/dev/null

assert_rpc_fails "createSubvolume — slash in name" "Bcachefs" "createSubvolume" \
    "{\"parent\":\"$MNT\",\"name\":\"bad/name\"}"

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
    DISK_BY_UUID="/dev/disk/by-uuid/$FS_UUID"
    if [ -e "$DISK_BY_UUID" ]; then
        assert_rpc "runScrubJob" "Bcachefs" "runScrubJob" \
            "{\"uuid\":\"$SCRUB_JOB_UUID\"}" >/dev/null
    else
        info "Skipping runScrubJob — $DISK_BY_UUID symlink not present"
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
    'group2name':        'hdd',
    'group2devices':     '$GROUP2_DEVS',
    'group3name':        '',
    'group3devices':     '',
    'foreground_target': 'ssd',
    'promote_target':    'ssd',
    'background_target': 'hdd',
    'compression':       'none',
    'checksum':          'crc32c',
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

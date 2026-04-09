#!/bin/sh

set -e

. /etc/default/openmediavault
. /usr/share/openmediavault/scripts/helper-functions

BCACHEFS_URI="https://apt.bcachefs.org/trixie"
BCACHEFS_KEY_FILE="/usr/share/openmediavault/bcachefs/apt.bcachefs.org.asc"

if ! omv_config_exists "/config/system/apt/sources/item[uris='${BCACHEFS_URI}']"; then
  echo "Adding bcachefs apt repository ..."
  SIGNEDBY=$(cat "${BCACHEFS_KEY_FILE}")
  jq --null-input --compact-output \
     --arg uuid "${OMV_CONFIGOBJECT_NEW_UUID}" \
     --arg uris "${BCACHEFS_URI}" \
     --arg signedby "${SIGNEDBY}" \
     '{"uuid": $uuid,
       "enable": "true",
       "types": "deb",
       "uris": $uris,
       "suites": "bcachefs-tools-release",
       "components": "main",
       "signedby": $signedby,
       "comment": "bcachefs-tools"}' | \
  omv-confdbadm update "conf.system.apt.source" -
fi

# Create the bcachefs service config tree for snapshot/scrub jobs.
if ! omv_config_exists "/config/services/bcachefs"; then
    omv_config_add_node "/config/services" "bcachefs"
fi
if ! omv_config_exists "/config/services/bcachefs/snapshotjobs"; then
    omv_config_add_node "/config/services/bcachefs" "snapshotjobs"
fi
if ! omv_config_exists "/config/services/bcachefs/scrubjobs"; then
    omv_config_add_node "/config/services/bcachefs" "scrubjobs"
fi

exit 0

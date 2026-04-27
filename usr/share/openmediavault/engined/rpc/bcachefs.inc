<?php
/**
 * Copyright (C) 2026 openmediavault plugin developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
namespace Engined\Rpc;

class Bcachefs extends \OMV\Rpc\ServiceAbstract
{
    public function getName()
    {
        return "Bcachefs";
    }

    public function initialize()
    {
        $this->registerMethod("getSettings");
        $this->registerMethod("setSettings");
        $this->registerMethod("createFilesystem");
        $this->registerMethod("getFilesystemList");
        $this->registerMethod("getFilesystem");
        $this->registerMethod("enableAutoUnlock");
        $this->registerMethod("disableAutoUnlock");
        $this->registerMethod("getSubvolumeList");
        $this->registerMethod("createSubvolume");
        $this->registerMethod("deleteSubvolume");
        $this->registerMethod("getSubvolumes");
        $this->registerMethod("getFilesystems");
        // Snapshot jobs
        $this->registerMethod("getSnapshotJobList");
        $this->registerMethod("getSnapshotJob");
        $this->registerMethod("setSnapshotJob");
        $this->registerMethod("deleteSnapshotJob");
        $this->registerMethod("runSnapshotJob");
        $this->registerMethod("runSnapshotJobBg");
        // Scrub jobs
        $this->registerMethod("getScrubJobList");
        $this->registerMethod("getScrubJob");
        $this->registerMethod("setScrubJob");
        $this->registerMethod("deleteScrubJob");
        $this->registerMethod("runScrubJob");
    }

    public function getSettings($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $db  = \OMV\Config\Database::getInstance();
        $obj = $db->get("conf.service.bcachefs");

        $kernelRelease = trim(shell_exec('uname -r') ?: '');
        $parts = explode('.', $kernelRelease);
        $major = intval($parts[0] ?? 0);
        $minor = intval(explode('-', $parts[1] ?? '0')[0]);
        $kernelOk = ($major > 6) || ($major === 6 && $minor >= 16);

        return [
            "suite"         => $obj->get("suite"),
            "kernelOk"      => $kernelOk,
            "kernelVersion" => $kernelRelease,
        ];
    }

    public function setSettings($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.setsettings");

        $db  = \OMV\Config\Database::getInstance();
        $obj = $db->get("conf.service.bcachefs");
        $obj->set("suite", $params['suite']);
        $db->set($obj);

        // Update the suite in the OMV apt source entry so the change
        // takes effect when the user applies changes.
        $filter = [
            'operator' => 'stringEquals',
            'arg0'     => 'uris',
            'arg1'     => 'https://apt.bcachefs.org/trixie',
        ];
        $aptObjects = $db->getByFilter("conf.system.apt.source", $filter);
        if (!empty($aptObjects)) {
            $aptObj = $aptObjects[0];
            $aptObj->set("suites", $params['suite']);
            $db->set($aptObj);
        }

        // Mark apt dirty so the sources file is regenerated, and bcachefs
        // dirty so bcachefs-tools is reinstalled from the new suite.
        $moduleMngr = \OMV\Engine\Module\Manager::getInstance();
        $moduleMngr->getModule("apt")->setDirty();
        $moduleMngr->getModule("bcachefs")->setDirty();

        return ["suite" => $params['suite']];
    }

    public function createFilesystem($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $params['nocow']        = filter_var($params['nocow']        ?? false, FILTER_VALIDATE_BOOLEAN);
        $params['encrypted']    = filter_var($params['encrypted']    ?? false, FILTER_VALIDATE_BOOLEAN);
        $params['autounlock']   = filter_var($params['autounlock']   ?? false, FILTER_VALIDATE_BOOLEAN);
        $params['enablegroups'] = filter_var($params['enablegroups'] ?? false, FILTER_VALIDATE_BOOLEAN);
        $params['replicas']     = intval($params['replicas'] ?? 1);
        $this->validateMethodParams($params, "rpc.bcachefs.createfilesystem");

        $nocow = $params['nocow'];
        $encrypted = !$nocow && $params['encrypted'];
        $autounlock = $encrypted && $params['autounlock'];
        if ($encrypted && empty($params['passphrase'])) {
            throw new \OMV\Exception(
                "A passphrase is required when encryption is enabled.");
        }

        // Build the ordered device list and per-device label map before
        // spawning the background process so we can validate early.
        $deviceFiles  = [];
        $deviceLabels = []; // device path => label string

        if ($params['enablegroups']) {
            $groupDefs = [
                ['name' => $params['group1name'] ?? '', 'devices' => $params['group1devices'] ?? ''],
                ['name' => $params['group2name'] ?? '', 'devices' => $params['group2devices'] ?? ''],
                ['name' => $params['group3name'] ?? '', 'devices' => $params['group3devices'] ?? ''],
            ];
            $seen = []; // device => group name, for duplicate detection
            foreach ($groupDefs as $g) {
                $gName = trim($g['name']);
                if ($gName === '' || trim($g['devices']) === '') continue;
                $n = 1;
                foreach (array_filter(array_map('trim', explode(',', $g['devices']))) as $dev) {
                    if (isset($seen[$dev])) {
                        throw new \OMV\Exception(
                            "Device '%s' is assigned to both group '%s' and group '%s'. " .
                            "Each device may only belong to one group.",
                            $dev, $seen[$dev], $gName);
                    }
                    $seen[$dev]           = $gName;
                    $deviceFiles[]        = $dev;
                    $deviceLabels[$dev]   = sprintf('%s.%s%d', $gName, $gName, $n++);
                }
            }
            if (empty($deviceFiles)) {
                throw new \OMV\Exception(
                    "At least one device group with a name and devices must be configured.");
            }
        } else {
            $deviceFiles = array_filter(array_map('trim', explode(',', $params['devicefiles'] ?? '')));
            if (empty($deviceFiles)) {
                throw new \OMV\Exception("At least one device must be selected.");
            }
        }

        if (count($deviceFiles) < $params['replicas']) {
            throw new \OMV\Exception(
                "The number of devices (%d) must be at least equal to the number of replicas (%d).",
                count($deviceFiles), $params['replicas']);
        }

        // Pre-generate a UUID so we can save the key file after format
        // when auto-unlock is requested.  Use the kernel's random UUID
        // source so no extra package (uuid-runtime) is required.
        $fsUuid = null;
        if ($autounlock) {
            $fsUuid = trim(file_get_contents('/proc/sys/kernel/random/uuid'));
        }

        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
          use ($params, $nocow, $encrypted, $autounlock, $fsUuid, $deviceFiles, $deviceLabels) {

            foreach ($deviceFiles as $deviceFile) {
                $sd = \OMV\System\Storage\StorageDevice::assertGetStorageDevice(
                    $deviceFile);
                $sd->wipe();
            }

            $cmdArgs = [];
            $cmdArgs[] = "format";
            $cmdArgs[] = "--force";

            if (!empty($params['label'])) {
                $cmdArgs[] = sprintf("--fs_label=%s", $params['label']);
            }
            if ($fsUuid !== null) {
                $cmdArgs[] = sprintf("--uuid=%s", $fsUuid);
            }
            if (!empty($params['replicas'])) {
                $cmdArgs[] = sprintf("--replicas=%d", intval($params['replicas']));
            }
            if (!empty($params['foreground_target'])) {
                $cmdArgs[] = sprintf("--foreground_target=%s", $params['foreground_target']);
            }
            if (!empty($params['promote_target'])) {
                $cmdArgs[] = sprintf("--promote_target=%s", $params['promote_target']);
            }
            if (!empty($params['background_target'])) {
                $cmdArgs[] = sprintf("--background_target=%s", $params['background_target']);
            }
            if (!empty($params['compression']) && $params['compression'] !== "none") {
                $cmdArgs[] = sprintf("--compression=%s", $params['compression']);
            }
            // Apply checksum only when it differs from the default (crc32c).
            $checksum = $params['checksum'] ?? "crc32c";
            if (!empty($checksum) && $checksum !== "crc32c") {
                $cmdArgs[] = sprintf("--data_checksum=%s", $checksum);
                $cmdArgs[] = sprintf("--metadata_checksum=%s", $checksum);
            }
            if ($nocow) {
                $cmdArgs[] = "--nocow";
            }

            // Write passphrase to a temporary file with tight permissions so
            // it is never visible on the process command line.
            $passphraseFile  = null;
            if ($encrypted) {
                $passphraseFile = tempnam(sys_get_temp_dir(), "bcachefs_");
                chmod($passphraseFile, 0600);
                file_put_contents($passphraseFile, $params['passphrase']);
                $cmdArgs[] = "--encrypted";
                $cmdArgs[] = sprintf("--passphrase_file=%s", $passphraseFile);
            }

            // Each --label flag must immediately precede the device it applies to.
            foreach ($deviceFiles as $deviceFile) {
                if (isset($deviceLabels[$deviceFile])) {
                    $cmdArgs[] = sprintf("--label=%s", $deviceLabels[$deviceFile]);
                }
                $cmdArgs[] = escapeshellarg($deviceFile);
            }

            try {
                $cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
                $cmd->setRedirect2to1();
                if (0 !== ($exitStatus = $this->exec($cmd, $output,
                        $bgOutputFilename))) {
                    throw new \OMV\ExecException($cmd, $output, $exitStatus);
                }
            } finally {
                if ($passphraseFile !== null && file_exists($passphraseFile)) {
                    unlink($passphraseFile);
                }
            }

            // Save the passphrase as an auto-unlock key file.
            if ($autounlock && $fsUuid !== null) {
                $keysDir = "/etc/bcachefs/keys";
                if (!is_dir($keysDir)) {
                    mkdir($keysDir, 0700, true);
                }
                $keyFile = sprintf("%s/%s.key", $keysDir, $fsUuid);
                file_put_contents($keyFile, $params['passphrase']);
                chmod($keyFile, 0600);
            }
        });
    }

    /**
     * Return all mounted bcachefs filesystems for the datatable.
     */
    public function getFilesystemList($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.common.getlist");
        $result = [];
        foreach ($this->getMountedBcachefsFilesystems() as $fs) {
            $keyFile    = sprintf("/etc/bcachefs/keys/%s.key", $fs['uuid']);
            $result[] = [
                "uuid"        => $fs['uuid'],
                "label"       => $fs['label'] ?? "",
                "mountpoint"  => $fs['mountpoint'],
                "encrypted"   => $fs['encrypted'],
                "autounlock"  => file_exists($keyFile),
                "devicecount" => $fs['devicecount'],
                "replicas"    => $fs['replicas'],
            ];
        }
        return $this->applyFilter($result, $params['start'],
            $params['limit'], $params['sortfield'], $params['sortdir']);
    }

    /**
     * Return a single filesystem entry (used by the auto-unlock form page).
     */
    public function getFilesystem($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.getfilesystem");
        foreach ($this->getMountedBcachefsFilesystems() as $fs) {
            if ($fs['uuid'] !== $params['uuid'])
                continue;
            $keyFile = sprintf("/etc/bcachefs/keys/%s.key", $fs['uuid']);
            return [
                "uuid"       => $fs['uuid'],
                "label"      => $fs['label'] ?? "",
                "mountpoint" => $fs['mountpoint'],
                "encrypted"  => $fs['encrypted'],
                "autounlock" => file_exists($keyFile),
            ];
        }
        throw new \OMV\Exception("Filesystem '%s' not found.", $params['uuid']);
    }

    /**
     * Save a passphrase key file to enable auto-unlock at boot.
     */
    public function enableAutoUnlock($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.enableautounlock");
        $keysDir = "/etc/bcachefs/keys";
        if (!is_dir($keysDir)) {
            mkdir($keysDir, 0700, true);
        }
        $keyFile = sprintf("%s/%s.key", $keysDir, $params['uuid']);
        file_put_contents($keyFile, $params['passphrase']);
        chmod($keyFile, 0600);
    }

    /**
     * Remove the key file to disable auto-unlock.
     */
    public function disableAutoUnlock($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.getfilesystem");
        $keyFile = sprintf("/etc/bcachefs/keys/%s.key", $params['uuid']);
        if (file_exists($keyFile)) {
            unlink($keyFile);
        }
    }

    /**
     * Return all subvolumes across all mounted bcachefs filesystems for the
     * datatable page. Includes snapshots and an in-use flag.
     */
    public function getSubvolumeList($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);

        // Build a map of absolute shared-folder paths for quick lookup.
        $usedPaths = [];
        try {
            $db = \OMV\Config\Database::getInstance();
            foreach ($db->get("conf.system.sharedfolder") as $sf) {
                $sfAssoc = $sf->getAssoc();
                try {
                    $me = $db->get("conf.system.filesystem.mountpoint",
                        $sfAssoc['mntentref']);
                    if ($me->get("type") !== "bcachefs")
                        continue;
                    $absPath = rtrim($me->get("dir"), '/') . '/' .
                        ltrim($sfAssoc['reldirpath'], '/');
                    $usedPaths[] = rtrim($absPath, '/');
                } catch (\Exception $e) {
                    continue;
                }
            }
        } catch (\Exception $e) {
            // No shared folders configured yet.
        }

        $isInUse = function(string $subvolPath) use ($usedPaths): bool {
            $subvolPath = rtrim($subvolPath, '/');
            foreach ($usedPaths as $used) {
                // In use if any shared folder is at or under this subvolume.
                if ($used === $subvolPath ||
                    strpos($used, $subvolPath . '/') === 0)
                    return true;
            }
            return false;
        };

        $result = [];
        foreach ($this->getMountedBcachefsFilesystems() as $fs) {
            $uuid = $fs['uuid'];
            $mnt  = $fs['mountpoint'];

            // Root subvolume is always in use (the filesystem mount itself).
            $result[] = [
                "path"       => $mnt,
                "relpath"    => "(root)",
                "mountpoint" => $mnt,
                "filesystem" => $uuid,
                "readonly"   => false,
                "issnapshot" => false,
                "inuse"      => true,
            ];

            $cmd = new \OMV\System\Process("bcachefs",
                ["subvolume", "list", "--json", "--snapshots",
                 "--recursive", escapeshellarg($mnt)]);
            $cmd->setRedirect2to1();
            $ioctlOk = false;
            try {
                $cmd->execute($output);
                $ioctlOk = true;
            } catch (\Exception $e) {
                // BCH_IOCTL_SUBVOLUME_LIST not supported by this kernel version.
            }

            if ($ioctlOk) {
                // Parse JSON output from newer bcachefs-tools.
                $subvols = json_decode(implode("", $output), true) ?? [];
                foreach ($subvols as $sv) {
                    $relpath = trim($sv['path'] ?? $sv['name'] ?? '');
                    if ($relpath === '' || $relpath === '/')
                        continue;
                    $absPath   = rtrim($mnt, '/') . '/' . ltrim($relpath, '/');
                    $isSnap    = !empty($sv['snapshot'] ?? $sv['flags']['snapshot'] ?? false)
                                  || str_starts_with($relpath, '.snapshots/');
                    $isRO      = !empty($sv['ro'] ?? $sv['readonly'] ?? $sv['flags']['ro'] ?? false);
                    $result[]  = [
                        "path"       => $absPath,
                        "relpath"    => $relpath,
                        "mountpoint" => $mnt,
                        "filesystem" => $uuid,
                        "readonly"   => $isRO,
                        "issnapshot" => $isSnap,
                        "inuse"      => $isInUse($absPath),
                    ];
                }
            } else {
                // Fallback: scan depth-1 directories for user subvolumes.
                foreach (@scandir($mnt) ?: [] as $entry) {
                    if ($entry === '.' || $entry === '..'
                            || str_starts_with($entry, '.'))
                        continue;
                    $absPath = rtrim($mnt, '/') . '/' . $entry;
                    if (!is_dir($absPath))
                        continue;
                    $result[] = [
                        "path"       => $absPath,
                        "relpath"    => $entry,
                        "mountpoint" => $mnt,
                        "filesystem" => $uuid,
                        "readonly"   => false,
                        "issnapshot" => false,
                        "inuse"      => $isInUse($absPath),
                    ];
                }
                // Scan .snapshots/<subvol>/<timestamp-prefix>/ for snapshots.
                $snapBase = rtrim($mnt, '/') . '/.snapshots';
                if (is_dir($snapBase)) {
                    foreach (@scandir($snapBase) ?: [] as $svName) {
                        if ($svName === '.' || $svName === '..')
                            continue;
                        $svDir = $snapBase . '/' . $svName;
                        if (!is_dir($svDir))
                            continue;
                        foreach (@scandir($svDir) ?: [] as $snapName) {
                            if ($snapName === '.' || $snapName === '..')
                                continue;
                            $snapPath = $svDir . '/' . $snapName;
                            if (!is_dir($snapPath))
                                continue;
                            $relpath  = '.snapshots/' . $svName . '/' . $snapName;
                            $result[] = [
                                "path"       => $snapPath,
                                "relpath"    => $relpath,
                                "mountpoint" => $mnt,
                                "filesystem" => $uuid,
                                "readonly"   => false,
                                "issnapshot" => true,
                                "inuse"      => $isInUse($snapPath),
                            ];
                        }
                    }
                }
            }
        }
        return $result;
    }

    /**
     * Create a bcachefs subvolume.
     */
    public function createSubvolume($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.createsubvolume");
        $path = rtrim($params['parent'], '/') . '/' . $params['name'];
        $cmd  = new \OMV\System\Process("bcachefs",
            ["subvolume", "create", escapeshellarg($path)]);
        $cmd->setRedirect2to1();
        $cmd->execute();
    }

    /**
     * Delete a bcachefs subvolume.
     */
    public function deleteSubvolume($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.deletesubvolume");
        $path = $params['path'];
        if (!file_exists($path) && !is_dir($path)) {
            throw new \OMV\Exception(
                "Subvolume path '%s' does not exist.", $path);
        }
        $cmd = new \OMV\System\Process("bcachefs",
            ["subvolume", "delete", escapeshellarg($path)]);
        $cmd->setRedirect2to1();
        try {
            $cmd->execute();
        } catch (\Exception $e) {
            // Fall back to rm -rf if bcachefs subvolume delete is unavailable.
            $cmd2 = new \OMV\System\Process("rm",
                ["-rf", "--", escapeshellarg($path)]);
            $cmd2->setRedirect2to1();
            $cmd2->execute();
        }
    }

    /**
     * Return non-snapshot subvolumes as [path, label] pairs for select fields.
     */
    public function getSubvolumes($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $result = [];
        foreach ($this->getMountedBcachefsFilesystems() as $fs) {
            $mnt    = $fs['mountpoint'];
            $prefix = $this->fsDisplayName($fs);

            $result[] = [$mnt, sprintf("%s: /", $prefix)];

            $cmd = new \OMV\System\Process("bcachefs",
                ["subvolume", "list", "--json", escapeshellarg($mnt)]);
            $cmd->setRedirect2to1();
            $ioctlOk = false;
            try {
                $cmd->execute($output);
                $ioctlOk = true;
            } catch (\Exception $e) {
                // BCH_IOCTL_SUBVOLUME_LIST not supported by this kernel version.
            }

            if ($ioctlOk) {
                $subvols = json_decode(implode("", $output), true) ?? [];
                foreach ($subvols as $sv) {
                    $relpath = trim($sv['path'] ?? $sv['name'] ?? '');
                    if ($relpath === '' || $relpath === '/')
                        continue;
                    // Skip snapshots — they are listed separately.
                    if (!empty($sv['snapshot'] ?? $sv['flags']['snapshot'] ?? false))
                        continue;
                    $result[] = [
                        rtrim($mnt, '/') . '/' . ltrim($relpath, '/'),
                        sprintf("%s: %s", $prefix, $relpath),
                    ];
                }
            } else {
                // Fallback: scan depth-1 non-hidden directories.
                foreach (@scandir($mnt) ?: [] as $entry) {
                    if ($entry === '.' || $entry === '..'
                            || str_starts_with($entry, '.'))
                        continue;
                    $absPath = rtrim($mnt, '/') . '/' . $entry;
                    if (!is_dir($absPath))
                        continue;
                    $result[] = [
                        $absPath,
                        sprintf("%s: %s", $prefix, $entry),
                    ];
                }
            }
        }
        return $result;
    }

    /**
     * Return mounted bcachefs filesystems as [uuid, label] pairs for select fields.
     */
    public function getFilesystems($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $result = [];
        foreach ($this->getMountedBcachefsFilesystems() as $fs) {
            $result[] = [
                $fs['uuid'],
                sprintf("%s (%s)", $this->fsDisplayName($fs), $fs['mountpoint']),
            ];
        }
        return $result;
    }

    // -------------------------------------------------------------------------
    // Snapshot jobs
    // -------------------------------------------------------------------------

    public function getSnapshotJobList($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.common.getlist");
        $db      = \OMV\Config\Database::getInstance();
        $objects = $db->get("conf.service.bcachefs.snapshotjob");
        $result  = array_map(fn($o) => $o->getAssoc(), $objects);
        return $this->applyFilter($result, $params['start'],
            $params['limit'], $params['sortfield'], $params['sortdir']);
    }

    public function getSnapshotJob($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.common.objectuuid");
        $db     = \OMV\Config\Database::getInstance();
        $obj    = $db->get("conf.service.bcachefs.snapshotjob", $params['uuid']);
        $result = $obj->getAssoc();
        foreach (['minute', 'hour', 'dayofmonth', 'month', 'dayofweek'] as $f) {
            $result[$f] = array_map('trim', explode(',', $result[$f]));
        }
        return $result;
    }

    public function setSnapshotJob($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.setsnapshotjob");
        foreach (['minute', 'hour', 'dayofmonth', 'month', 'dayofweek'] as $f) {
            if (is_array($params[$f]))
                $params[$f] = implode(',', $params[$f]);
        }
        $db  = \OMV\Config\Database::getInstance();
        $obj = new \OMV\Config\ConfigObject("conf.service.bcachefs.snapshotjob");
        $obj->setAssoc($params);
        $db->set($obj);
        return $obj->getAssoc();
    }

    public function deleteSnapshotJob($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.common.objectuuid");
        $db  = \OMV\Config\Database::getInstance();
        $obj = $db->get("conf.service.bcachefs.snapshotjob", $params['uuid']);
        $db->delete($obj);
    }

    public function runSnapshotJob($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.common.objectuuid");
        $db  = \OMV\Config\Database::getInstance();
        $obj = $db->get("conf.service.bcachefs.snapshotjob", $params['uuid']);
        $j   = $obj->getAssoc();

        $retentionUnit = $j['retentionunit'] ?? 'count';
        $unitFlag = ($retentionUnit !== 'count')
            ? sprintf(' -u %s', escapeshellarg($retentionUnit)) : '';
        $readonlyFlag = !empty($j['readonly']) ? ' -r' : '';

        $cmd = sprintf('/usr/sbin/omv-bcachefs-snapshot %s %s %d%s%s%s 2>&1',
            escapeshellarg($j['subvolume']),
            escapeshellarg($j['prefix']),
            (int)$j['retention'],
            $unitFlag,
            $readonlyFlag,
            !empty($j['sendemail'])
                ? sprintf(' -e %s%s',
                    $j['emailonerror'] ? 'onerror' : 'always',
                    !empty($j['comment'])
                        ? ' -c ' . escapeshellarg($j['comment']) : '')
                : '');
        exec($cmd, $out, $res);
        if ($res !== 0)
            throw new \OMV\Exception('Snapshot job failed: ' . implode("\n", $out));
        return implode("\n", $out);
    }

    public function runSnapshotJobBg($params, $context)
    {
        return $this->callMethodBg('runSnapshotJob', $params, $context);
    }

    // -------------------------------------------------------------------------
    // Scrub jobs
    // -------------------------------------------------------------------------

    public function getScrubJobList($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.common.getlist");
        $db      = \OMV\Config\Database::getInstance();
        $objects = $db->get("conf.service.bcachefs.scrubjob");
        $result  = array_map(fn($o) => $o->getAssoc(), $objects);
        return $this->applyFilter($result, $params['start'],
            $params['limit'], $params['sortfield'], $params['sortdir']);
    }

    public function getScrubJob($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.common.objectuuid");
        $db     = \OMV\Config\Database::getInstance();
        $obj    = $db->get("conf.service.bcachefs.scrubjob", $params['uuid']);
        $result = $obj->getAssoc();
        foreach (['minute', 'hour', 'dayofmonth', 'month', 'dayofweek'] as $f) {
            $result[$f] = array_map('trim', explode(',', $result[$f]));
        }
        return $result;
    }

    public function setScrubJob($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.setscrubjob");
        // Accept 'filesystem' as an alias for backwards compatibility with
        // any stored data that used the old field name.
        if (isset($params['filesystem']) && !isset($params['fsuuid'])) {
            $params['fsuuid'] = $params['filesystem'];
        }
        unset($params['filesystem']);
        foreach (['minute', 'hour', 'dayofmonth', 'month', 'dayofweek'] as $f) {
            if (is_array($params[$f]))
                $params[$f] = implode(',', $params[$f]);
        }
        $db  = \OMV\Config\Database::getInstance();
        $obj = new \OMV\Config\ConfigObject("conf.service.bcachefs.scrubjob");
        $obj->setAssoc($params);
        $db->set($obj);
        return $obj->getAssoc();
    }

    public function deleteScrubJob($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.common.objectuuid");
        $db  = \OMV\Config\Database::getInstance();
        $obj = $db->get("conf.service.bcachefs.scrubjob", $params['uuid']);
        $db->delete($obj);
    }

    public function runScrubJob($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.common.objectuuid");
        $db  = \OMV\Config\Database::getInstance();
        $obj = $db->get("conf.service.bcachefs.scrubjob", $params['uuid']);
        $j   = $obj->getAssoc();

        $cmd = sprintf('/usr/sbin/omv-bcachefs-scrub %s%s 2>&1',
            escapeshellarg($j['fsuuid']),
            !empty($j['sendemail'])
                ? sprintf(' -e %s%s',
                    $j['emailonerror'] ? 'onerror' : 'always',
                    !empty($j['comment'])
                        ? ' -c ' . escapeshellarg($j['comment']) : '')
                : '');
        exec($cmd, $out, $res);
        if ($res !== 0)
            throw new \OMV\Exception('Scrub job failed: ' . implode("\n", $out));
        return implode("\n", $out);
    }

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    /**
     * Scan /proc/mounts and return all mounted bcachefs filesystems with
     * UUID, mount point, first device, label, and encryption status.
     */
    private function getMountedBcachefsFilesystems(): array
    {
        $result = [];
        $seen   = [];
        $lines  = file("/proc/mounts",
            FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
        foreach ($lines as $line) {
            $parts = preg_split('/\s+/', $line);
            if (!isset($parts[2]) || $parts[2] !== "bcachefs")
                continue;
            $mnt = unescape_path($parts[1]);
            // OMV mounts bcachefs at /srv/dev-disk-by-uuid-<UUID>.
            if (!preg_match(
                '/dev-disk-by-uuid-([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})/',
                $mnt, $m))
                continue;
            $uuid = $m[1];
            if (isset($seen[$uuid]))
                continue;
            $seen[$uuid] = true;

            // For multi-device filesystems /proc/mounts uses "dev1:dev2:..."
            $allDevices  = explode(':', $parts[0]);
            $firstDevice = $allDevices[0];

            // Query the superblock for the filesystem label and encryption flag.
            // Try each device in turn; on multi-device arrays the first listed
            // device may not always be readable (e.g. permission races at mount
            // time), so fall through to the next until one succeeds.
            $label       = null;
            $encrypted   = false;
            $replicas    = 1;
            foreach ($allDevices as $device) {
                // Use exec() directly so we capture output regardless of exit
                // code.  bcachefs show-super exits non-zero on some device types
                // (e.g. NVMe-oF mapper devices) even when it outputs valid data,
                // which causes OMV\System\Process::execute() to throw and discard
                // the output before we can parse it.
                $superLines = [];
                exec('/usr/sbin/bcachefs show-super ' .
                    escapeshellarg($device) . ' 2>&1', $superLines);
                if (empty($superLines))
                    continue;
                $superText = implode("\n", $superLines);
                if (preg_match('/^Label:\s+(.+)$/m', $superText, $lm)) {
                    $lv    = trim($lm[1]);
                    $label = ($lv === '(none)' || $lv === '') ? null : $lv;
                }
                // The Features: line lists active features; 'encrypt' appears
                // when whole-filesystem encryption is enabled.
                if (preg_match('/^Features:.*\bencrypt\b/m', $superText)) {
                    $encrypted = true;
                }
                if (preg_match('/^\s*data_replicas:\s+(\d+)/m', $superText, $rm)) {
                    $replicas = intval($rm[1]);
                }
                break; // Superblock read succeeded; stop trying devices.
            }

            $result[] = [
                "uuid"        => $uuid,
                "mountpoint"  => $mnt,
                "device"      => $firstDevice,
                "devicecount" => count($allDevices),
                "label"       => $label,
                "encrypted"   => $encrypted,
                "replicas"    => $replicas,
            ];
        }
        return $result;
    }

    /**
     * Return a short display name for a filesystem entry: the label if set,
     * otherwise the first 8 characters of the UUID.
     */
    private function fsDisplayName(array $fs): string
    {
        return $fs['label'] ?? substr($fs['uuid'], 0, 8);
    }
}

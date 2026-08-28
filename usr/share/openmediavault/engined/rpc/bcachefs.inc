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
        $this->registerMethod("getEmptyCandidates");
        $this->registerMethod("getFilesystemList");
        $this->registerMethod("getFilesystem");
        $this->registerMethod("enableAutoUnlock");
        $this->registerMethod("disableAutoUnlock");
        $this->registerMethod("getSubvolumeList");
        $this->registerMethod("createSubvolume");
        $this->registerMethod("deleteSubvolume");
        $this->registerMethod("getSubvolumes");
        $this->registerMethod("getFilesystems");
        // Filesystem usage / options
        $this->registerMethod("getUsage");
        $this->registerMethod("getFilesystemOptions");
        $this->registerMethod("setFilesystemOptions");
        // Device management
        $this->registerMethod("getDeviceList");
        $this->registerMethod("addDevice");
        $this->registerMethod("removeDevice");
        $this->registerMethod("evacuateDevice");
        $this->registerMethod("setDeviceState");
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
        $arch = trim(shell_exec('dpkg --print-architecture 2>/dev/null') ?: '');

        // bcachefs is no longer maintained in mainline, so the module always
        // comes from one of two out-of-tree providers. Report which one is in
        // effect for the running kernel so the UI can explain the situation.
        //   prebuilt - a precompiled .ko from module.bcachefs.org (installed to
        //              updates/bcachefs.ko by omv-bcachefs-module).
        //   dkms     - built locally by bcachefs-kernel-dkms (installs under
        //              updates/dkms/), required for arm64 and the Proxmox kernel.
        //   none     - no module yet; Apply changes to install one.
        $moduleLoaded = ('1' === trim(shell_exec(
            "lsmod | awk '\$1 == \"bcachefs\" { print 1; exit }'") ?: ''));
        $prebuilt = ('' !== $kernelRelease) && file_exists(
            "/lib/modules/{$kernelRelease}/updates/bcachefs.ko");
        $dkmsInstalled = (false !== strpos(trim(shell_exec(
            "dpkg-query -W -f='\${Status}' bcachefs-kernel-dkms 2>/dev/null") ?: ''),
            'install ok installed'));

        if ($prebuilt) {
            $moduleSource = 'prebuilt';
        } elseif ($dkmsInstalled) {
            $moduleSource = 'dkms';
        } else {
            $moduleSource = 'none';
        }

        // Generic build verification: is a loadable bcachefs module actually
        // present for the running kernel? This catches the case where DKMS is
        // installed but the build produced nothing (e.g. a real C build break
        // or missing headers). Note a missing Rust toolchain is NOT a failure:
        // the DKMS build falls back to a C-only module on its own.
        $rc = 1;
        if ('' !== $kernelRelease) {
            $dummy = [];
            @exec('modinfo -k ' . escapeshellarg($kernelRelease) .
                ' bcachefs 2>/dev/null', $dummy, $rc);
        }
        $moduleBuilt = $moduleLoaded || (0 === $rc);

        return [
            "suite"         => $obj->get("suite"),
            "kernelVersion" => $kernelRelease,
            "arch"          => $arch,
            "moduleLoaded"  => $moduleLoaded,
            "moduleBuilt"   => $moduleBuilt,
            "moduleSource"  => $moduleSource,
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
        $params['erasurecode']  = filter_var($params['erasurecode']  ?? false, FILTER_VALIDATE_BOOLEAN);
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
            if (!empty($params['erasurecode'])) {
                $cmdArgs[] = "--erasure_code";
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

    public function getEmptyCandidates($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        if (false === ($devs = \OMV\System\Storage\StorageDevice::enumerateUnused())) {
            throw new \OMV\Exception("Failed to get list of unused devices.");
        }
        $result = [];
        $db = \OMV\Config\Database::getInstance();
        foreach ($devs as $devv) {
            $sd = \OMV\System\Storage\StorageDevice::getStorageDevice($devv);
            if (is_null($sd) || !$sd->exists()) {
                continue;
            }
            // Skip read-only and USB devices.
            if ($sd->isReadOnly() || $sd->isUsb()) {
                continue;
            }
            // Skip devices already referenced by a configured service.
            if ($db->exists("conf.service", [
                "operator" => "stringContains",
                "arg0"     => "devicefile",
                "arg1"     => $sd->getDeviceFile(),
            ])) {
                continue;
            }
            // Skip devices that already contain a filesystem.
            if (false !== \OMV\System\Filesystem\Filesystem::hasFileSystem($sd->getDeviceFile())) {
                continue;
            }
            // Prefer the stable by-id path over the kernel device name to
            // avoid mixed naming when attaching or replacing devices.
            $devfile = $sd->getDeviceFile();
            if (!is_null($byId = $sd->getDeviceFileById())) {
                $devfile = $byId;
            }
            $sn = $sd->getSerialNumber();
            $result[] = [
                "devicefile"   => $devfile,
                "size"         => $sd->getSize(),
                "vendor"       => $sd->getVendor(),
                "serialnumber" => $sn,
                "description"  => $sd->getDescription(),
                "label"        => $sn
                                    ? $sd->getDescription() . ", S/N: $sn"
                                    : $sd->getDescription(),
            ];
        }
        return $result;
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
            $mnt        = $fs['mountpoint'];
            // Report usable capacity as seen by statvfs, which already accounts
            // for replication and compression overhead. Mirrors the figures the
            // core Filesystems page shows.
            $total      = @disk_total_space($mnt);
            $free       = @disk_free_space($mnt);
            $used       = ($total !== false && $free !== false)
                            ? max(0.0, $total - $free) : null;
            $usedpercent = ($total) ? round($used / $total * 100, 1) : 0;
            $result[] = [
                "uuid"        => $fs['uuid'],
                "label"       => $fs['label'] ?? "",
                "mountpoint"  => $mnt,
                "encrypted"   => $fs['encrypted'],
                "autounlock"  => file_exists($keyFile),
                "devicecount" => $fs['devicecount'],
                "replicas"    => $fs['replicas'],
                "total"       => ($total !== false) ? $total : null,
                "used"        => $used,
                "usedpercent" => $usedpercent,
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
                "usage"      => "",
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
                    // Newer bcachefs-tools report per-subvolume/snapshot disk
                    // usage; surface it when present (key name varies by version).
                    $usageRaw  = $sv['usage'] ?? $sv['disk_usage'] ?? $sv['size'] ?? null;
                    $result[]  = [
                        "path"       => $absPath,
                        "relpath"    => $relpath,
                        "mountpoint" => $mnt,
                        "filesystem" => $uuid,
                        "readonly"   => $isRO,
                        "issnapshot" => $isSnap,
                        "inuse"      => $isInUse($absPath),
                        "usage"      => is_numeric($usageRaw)
                                          ? binary_format((float)$usageRaw)
                                          : (string)($usageRaw ?? ""),
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
                        "usage"      => "",
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
                                "usage"      => "",
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
    // Filesystem usage / options
    // -------------------------------------------------------------------------

    /**
     * Return the human-readable `bcachefs fs usage` report for a filesystem,
     * with reconcile/rebalance status appended when the running tools support
     * it. Used by the read-only usage/health view.
     */
    public function getUsage($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.getfilesystem");
        $fs  = $this->getFsByUuid($params['uuid']);
        $mnt = $fs['mountpoint'];

        $out = [];
        exec('/usr/sbin/bcachefs fs usage -h ' . escapeshellarg($mnt) .
            ' 2>&1', $out);
        $report = implode("\n", $out);

        // Reconcile replaces the older rebalance reporting; only present on
        // newer tools, so ignore failures.
        $rec = [];
        exec('/usr/sbin/bcachefs reconcile status ' . escapeshellarg($mnt) .
            ' 2>/dev/null', $rec, $rc);
        if ($rc === 0 && !empty($rec)) {
            $report .= "\n\n=== Reconcile status ===\n" . implode("\n", $rec);
        }

        return [
            "uuid"       => $fs['uuid'],
            "label"      => $fs['label'] ?? "",
            "mountpoint" => $mnt,
            "report"     => $report,
        ];
    }

    /**
     * Read the current IO-path options for a mounted filesystem from sysfs.
     * Changing these triggers bcachefs reconcile to migrate existing data.
     */
    public function getFilesystemOptions($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.getfilesystem");
        $fs   = $this->getFsByUuid($params['uuid']);
        $uuid = $fs['uuid'];
        $base = sprintf("/sys/fs/bcachefs/%s/options/", $uuid);
        $read = function(string $name) use ($base): string {
            $f = $base . $name;
            return is_readable($f) ? trim((string)@file_get_contents($f)) : "";
        };
        // The compression option may carry a level suffix (e.g. "zstd:3");
        // the select field only knows the algorithm name.
        $compression = explode(':', $read('compression'))[0];
        if ($compression === '') {
            $compression = 'none';
        }
        return [
            "uuid"                   => $uuid,
            "label"                  => $fs['label'] ?? "",
            "mountpoint"             => $fs['mountpoint'],
            "compression"            => $compression,
            "background_compression" => explode(':', $read('background_compression'))[0] ?: 'none',
            "foreground_target"      => $read('foreground_target'),
            "background_target"      => $read('background_target'),
            "promote_target"         => $read('promote_target'),
            "data_replicas"          => intval($read('data_replicas')) ?: 1,
        ];
    }

    /**
     * Apply changed IO-path options to a mounted filesystem via sysfs.
     * bcachefs persists these to the superblock automatically.
     */
    public function setFilesystemOptions($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.setfilesystemoptions");
        $base   = sprintf("/sys/fs/bcachefs/%s/options/", $params['uuid']);
        $errors = [];
        $write  = function(string $name, $value) use ($base, &$errors) {
            if ($value === '' || $value === null) {
                return; // Leave blank fields unchanged.
            }
            if (false === @file_put_contents($base . $name, (string)$value)) {
                $errors[] = sprintf("%s=%s", $name, $value);
            }
        };
        $write('compression',            $params['compression'] ?? '');
        $write('background_compression', $params['background_compression'] ?? '');
        $write('foreground_target',      $params['foreground_target'] ?? '');
        $write('background_target',      $params['background_target'] ?? '');
        $write('promote_target',         $params['promote_target'] ?? '');
        if (isset($params['data_replicas'])) {
            $write('data_replicas', (string)intval($params['data_replicas']));
        }
        if (!empty($errors)) {
            throw new \OMV\Exception(
                "Failed to apply option(s): %s. Target group names must " .
                "already exist on the filesystem.", implode(', ', $errors));
        }
    }

    // -------------------------------------------------------------------------
    // Device management
    // -------------------------------------------------------------------------

    /**
     * List every member device across all mounted bcachefs filesystems, with
     * its label group, state and size parsed from the superblock.
     */
    public function getDeviceList($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $result = [];
        foreach ($this->getMountedBcachefsFilesystems() as $fs) {
            $devices = $fs['devices'];

            // Read the superblock once from whichever device responds first.
            $superText = "";
            foreach ($devices as $d) {
                $lines = [];
                exec('/usr/sbin/bcachefs show-super ' . escapeshellarg($d) .
                    ' 2>/dev/null', $lines);
                if (!empty($lines)) {
                    $superText = implode("\n", $lines);
                    break;
                }
            }
            $members = $this->parseSuperMembers($superText);

            // Members are listed in member-index order; the device list from
            // /proc/mounts is assumed to follow the same order. Operations
            // always target the real device path shown in the row.
            $i = 0;
            foreach ($devices as $d) {
                $mem = $members[$i] ?? [
                    'label' => '', 'state' => '', 'size' => '', 'durability' => '',
                ];
                $result[] = [
                    "devicefile" => $d,
                    "filesystem" => $fs['uuid'],
                    "fslabel"    => $this->fsDisplayName($fs),
                    "mountpoint" => $fs['mountpoint'],
                    "group"      => $mem['label'],
                    "state"      => $mem['state'] !== '' ? $mem['state'] : 'unknown',
                    "size"       => $mem['size'],
                    "durability" => $mem['durability'],
                ];
                $i++;
            }
        }
        return $result;
    }

    /**
     * Add a new (empty) device to an existing filesystem, optionally placing it
     * in a label group. Runs in the background as data may be rebalanced.
     */
    public function addDevice($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.adddevice");
        $fs  = $this->getFsByUuid($params['uuid']);
        $dev = $params['devicefile'];
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
          use ($fs, $dev, $params) {
            $sd = \OMV\System\Storage\StorageDevice::assertGetStorageDevice($dev);
            $sd->wipe();

            // The device was just wiped, so overwrite any residual signature
            // (e.g. a bcachefs superblock that wipefs does not recognise).
            $cmdArgs = ["device", "add", "--force"];
            if (!empty($params['label'])) {
                $cmdArgs[] = sprintf("--label=%s", escapeshellarg($params['label']));
            }
            $cmdArgs[] = escapeshellarg($fs['mountpoint']);
            $cmdArgs[] = escapeshellarg($dev);

            $cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
            $cmd->setRedirect2to1();
            if (0 !== ($exitStatus = $this->exec($cmd, $output,
                    $bgOutputFilename))) {
                throw new \OMV\ExecException($cmd, $output, $exitStatus);
            }
        });
    }

    /**
     * Remove a device from its filesystem. Data is migrated off first, so this
     * runs in the background.
     */
    public function removeDevice($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.devicefile");
        $dev = $params['devicefile'];
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
          use ($dev) {
            $cmd = new \OMV\System\Process("bcachefs",
                ["device", "remove", escapeshellarg($dev)]);
            $cmd->setRedirect2to1();
            if (0 !== ($exitStatus = $this->exec($cmd, $output,
                    $bgOutputFilename))) {
                throw new \OMV\ExecException($cmd, $output, $exitStatus);
            }
        });
    }

    /**
     * Move all data off a device without removing it from the filesystem.
     * Runs in the background.
     */
    public function evacuateDevice($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.devicefile");
        $dev = $params['devicefile'];
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
          use ($dev) {
            $cmd = new \OMV\System\Process("bcachefs",
                ["device", "evacuate", escapeshellarg($dev)]);
            $cmd->setRedirect2to1();
            if (0 !== ($exitStatus = $this->exec($cmd, $output,
                    $bgOutputFilename))) {
                throw new \OMV\ExecException($cmd, $output, $exitStatus);
            }
        });
    }

    /**
     * Change a device's state (rw, ro, failed/evacuating, spare).
     */
    public function setDeviceState($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->validateMethodParams($params, "rpc.bcachefs.setdevicestate");
        $state = $params['state'];
        $dev   = $params['devicefile'];
        // Newer bcachefs-tools renamed the "failed" device state to
        // "evacuating" in `bcachefs device set-state` (valid values became
        // rw, ro, evacuating, spare). Accept either name from the UI and send
        // whichever token the installed tool understands.
        if (in_array($state, ['failed', 'evacuating'], true)) {
            $state = $this->bcachefsFailedStateToken();
        }
        $cmdArgs = ["device", "set-state"];
        // Forcing past the safety check is required to take a device out of
        // service (failed/evacuating) or mark it spare while it still holds
        // data.
        if (in_array($state, ['failed', 'evacuating', 'spare'], true)) {
            $cmdArgs[] = "--force";
        }
        $cmdArgs[] = $state;
        $cmdArgs[] = escapeshellarg($dev);
        $cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
        $cmd->setRedirect2to1();
        $cmd->execute();
    }

    /**
     * Return the token the installed bcachefs-tools uses for the "stop using
     * this device" state: "evacuating" on newer releases, "failed" on older
     * ones. Falls back to "failed" when the tool cannot be probed.
     */
    private function bcachefsFailedStateToken(): string
    {
        $out = [];
        @exec('/usr/sbin/bcachefs device set-state --help 2>&1', $out);
        return (false !== stripos(implode("\n", $out), 'evacuating'))
            ? 'evacuating' : 'failed';
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
     * Scan /proc/mounts for mounted bcachefs filesystems and return each with
     * its UUID, mount point, member device list (from sysfs), label, and
     * encryption status.
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

            // Enumerate member devices from sysfs. Current bcachefs kernels no
            // longer expose the colon-separated member list in /proc/mounts —
            // the source there is frequently a single path such as
            // /dev/disk/by-uuid/<UUID> — so /sys/fs/bcachefs/<uuid>/dev-*/block
            // is the reliable source. The dev-N index matches the superblock
            // member order.
            $allDevices = [];
            foreach (glob("/sys/fs/bcachefs/{$uuid}/dev-*", GLOB_ONLYDIR) ?: []
                    as $devDir) {
                if (!preg_match('#/dev-(\d+)$#', $devDir, $dm))
                    continue;
                $target = @readlink("{$devDir}/block");
                if ($target === false || $target === null)
                    continue;
                $kname  = basename($target);
                $dmName = @file_get_contents("/sys/block/{$kname}/dm/name");
                $allDevices[intval($dm[1])] =
                    (is_string($dmName) && trim($dmName) !== '')
                        ? "/dev/mapper/" . trim($dmName)
                        : "/dev/" . $kname;
            }
            ksort($allDevices);
            $allDevices = array_values($allDevices);

            // Older kernels listed the members colon-separated in /proc/mounts;
            // fall back to that when sysfs gave us nothing.
            if (empty($allDevices))
                $allDevices = explode(':', $parts[0]);
            $firstDevice = $allDevices[0] ?? $parts[0];

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
                "devices"     => $allDevices,
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

    /**
     * Look up a mounted filesystem entry by UUID, or throw if not found.
     */
    private function getFsByUuid(string $uuid): array
    {
        foreach ($this->getMountedBcachefsFilesystems() as $fs) {
            if ($fs['uuid'] === $uuid) {
                return $fs;
            }
        }
        throw new \OMV\Exception("Filesystem '%s' not found.", $uuid);
    }

    /**
     * Parse the per-device members block of `bcachefs show-super` output into
     * an array indexed by member number, each with label, state, size and
     * durability. Fields are only captured once a "Device:" header has been
     * seen so the filesystem-level Label line is not mistaken for a member.
     */
    private function parseSuperMembers(string $superText): array
    {
        $members = [];
        $idx     = null;
        foreach (preg_split('/\r?\n/', $superText) as $line) {
            // The device header reads "Device 0:  /dev/...  (model)".
            if (preg_match('/^\s*Device\s+(\d+):/', $line, $m)) {
                $idx = intval($m[1]);
                $members[$idx] = [
                    'label' => '', 'state' => '', 'size' => '', 'durability' => '',
                ];
                continue;
            }
            if ($idx === null) {
                continue;
            }
            if (preg_match('/^\s*Label:\s*(.+?)\s*$/', $line, $m)) {
                $members[$idx]['label'] =
                    ($m[1] === '(none)') ? '' : $m[1];
            } elseif (preg_match('/^\s*Size:\s*(.+?)\s*$/', $line, $m)) {
                $members[$idx]['size'] = $m[1];
            } elseif (preg_match('/^\s*Durability:\s*(.+?)\s*$/', $line, $m)) {
                $members[$idx]['durability'] = $m[1];
            } elseif (preg_match('/^\s*State:\s*(.+?)\s*$/', $line, $m)) {
                $members[$idx]['state'] = $m[1];
            }
        }
        return $members;
    }
}

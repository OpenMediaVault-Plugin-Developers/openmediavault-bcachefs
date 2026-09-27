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
namespace OMV\System\Filesystem;

class Bcachefs extends Filesystem {
	protected $numTotalDeviceFiles = 0;
	protected $deviceFiles = [];

	/**
	 * For multi-device bcachefs the kernel reports the SOURCE in /proc/mounts
	 * as "/dev/sdb:/dev/sdc" (colon-separated). blkid cannot handle that
	 * string, so we reduce it to the first device before delegating to the
	 * parent, which then runs blkid on a real block device path.
	 */
	protected function getData() {
		if (FALSE !== $this->isCached())
			return;

		// If the stored device file is a multi-device path, keep only the
		// first component so that blkid in the parent can probe it.
		if (strpos($this->deviceFile, ':') !== FALSE) {
			$devices = explode(':', $this->deviceFile);
			$this->deviceFile = $devices[0];
		}

		parent::getData();

		if (!$this->hasUuid())
			return;

		$this->setCached(FALSE);

		// Get per-device information from bcachefs.
		// bcachefs fs usage <uuid>
		$cmdArgs = [];
		$cmdArgs[] = "fs";
		$cmdArgs[] = "usage";
		$cmdArgs[] = escapeshellarg(sprintf("/dev/disk/by-uuid/%s", $this->uuid));
		$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
		$cmd->setRedirect2to1();
		try {
			$cmd->execute($output);
		} catch (\Exception $e) {
			return;
		}

		// bcachefs reports only the short device name (kernel name), not the
		// full path.  Prepend /dev/ and resolve to the canonical path so the
		// result matches what StorageDevice::getCanonicalDeviceFile() returns.
		// Offline members count towards the total (like btrfs "missing"
		// devices) but have no device file.
		$this->deviceFiles = [];
		$members = self::parseUsageDevices($output);
		foreach ($members as $member) {
			if (!$member['online'])
				continue;
			$fullPath  = '/dev/' . $member['dev'];
			$this->deviceFiles[] = realpath($fullPath) ?: $fullPath;
		}
		$this->numTotalDeviceFiles = count($members);
		$this->setCached(TRUE);
	}

	/**
	 * Parse the member lines of `bcachefs fs usage` output, e.g.
	 *   (no label) (device 0):  dm-18   rw       ...
	 *   ssd.ssd1   (device 1):  sda     rw       ...
	 *   hdd.hdd1   (device 2):  dev-2   offline  ...
	 * Since bcachefs-tools 1.39.3 a member that is listed in the superblock
	 * but not present is reported with the state "offline" and, having no
	 * block device, the sysfs directory name (dev-N) in place of a kernel
	 * device name.
	 *
	 * @param array $output The output lines.
	 * @return array A list of ['idx' => int, 'dev' => string,
	 *   'online' => bool], one per member.
	 */
	public static function parseUsageDevices(array $output): array {
		$result = [];
		$regex = '/\(device\s+(\d+)\):\s+(\S+)(?:\s+(\S+))?/';
		foreach ($output as $line) {
			if (!preg_match($regex, $line, $matches))
				continue;
			$result[] = [
				'idx'    => intval($matches[1]),
				'dev'    => $matches[2],
				'online' => ($matches[3] ?? '') !== 'offline',
			];
		}
		return $result;
	}

	/**
	 * Parse the output of `bcachefs damage ls [-R]` (bcachefs-tools 1.39.5
	 * and newer). Each line is a path relative to the listed directory,
	 * followed by ": " and the space-separated error names recorded
	 * against it when they could be read, e.g.
	 *   videos/holiday.mkv: ptr_to_missing_device extent_csum_bad
	 *   docs/report.pdf
	 * Error names are lower-case identifiers, which is what tells a
	 * trailing error list apart from a ": " inside a file name. A file whose
	 * own name ends in ": <identifier>" is still ambiguous, so the raw line
	 * is returned too for callers that can check which one exists.
	 *
	 * @param array $output The output lines.
	 * @return array A list of ['path' => string, 'errors' => string[],
	 *   'line' => string].
	 */
	public static function parseDamageList(array $output): array {
		$result = [];
		foreach ($output as $line) {
			$line = rtrim($line, "\r\n");
			if ($line === '')
				continue;
			if (preg_match('/^(.+): ([a-z0-9_]+(?: [a-z0-9_]+)*)$/',
					$line, $matches)) {
				$result[] = [
					'path'   => $matches[1],
					'errors' => explode(' ', $matches[2]),
					'line'   => $line,
				];
			} else {
				$result[] = ['path' => $line, 'errors' => [], 'line' => $line];
			}
		}
		return $result;
	}

	public function refresh() {
		parent::refresh();
		$this->setCached(FALSE);
		$this->getData();
	}

	public function getDeviceFiles() {
		$this->getData();
		$deviceFiles = array_filter(array_values($this->deviceFiles),
		  function($value) { return !empty($value); });
		sort($deviceFiles, SORT_NATURAL);
		return $deviceFiles;
	}

	public function getNumTotalDeviceFiles() {
		$this->getData();
		return $this->numTotalDeviceFiles;
	}

	/**
	 * findmnt leaves the UUID column empty for bcachefs and multi-device
	 * SOURCE paths (e.g. /dev/dm-19:/dev/dm-20) are not valid file paths,
	 * so the parent matching logic fails on both checks. We search
	 * /proc/mounts directly for a bcachefs entry whose mount point contains
	 * our UUID (OMV always mounts to /srv/dev-disk-by-uuid-<UUID>).
	 */
	public function getMountPoint() {
		$this->getData();
		if ($this->hasUuid()) {
			$lines = file("/proc/mounts", FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
			foreach ($lines as $line) {
				$parts = preg_split('/\s+/', $line);
				// parts: device mountpoint fstype options dump pass
				if (!isset($parts[2]) || $parts[2] !== "bcachefs")
					continue;
				$mountPoint = unescape_path($parts[1]);
				// OMV encodes the UUID in the mount point directory name.
				if (strpos($mountPoint, $this->getUuid()) !== FALSE)
					return $mountPoint;
			}
		}
		return parent::getMountPoint();
	}

	/**
	 * Multi-device bcachefs mounts use a colon-separated SOURCE in the
	 * kernel mount table (e.g. /dev/dm-19:/dev/dm-20), which does not
	 * match any single device file. Always unmount by directory.
	 */
	public function umount($force = FALSE, $lazy = FALSE, $directory = FALSE) {
		parent::umount($force, $lazy, TRUE);
	}

	public function getStatistics() {
		$this->getData();
		return parent::getStatistics();
	}

	public function fsck($fix = FALSE, $verbose = FALSE) {
		$cmdArgs = [];
		$cmdArgs[] = "fsck";
		if ($verbose)
			$cmdArgs[] = "-v";
		$cmdArgs[] = escapeshellarg(sprintf("/dev/disk/by-uuid/%s", $this->uuid));
		$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
		$cmd->setRedirect2to1();
		$cmd->execute();
	}

	public function scrub() {
		$cmdArgs = [];
		$cmdArgs[] = "data";
		$cmdArgs[] = "scrub";
		$cmdArgs[] = "start";
		$cmdArgs[] = "-b";
		$cmdArgs[] = escapeshellarg(sprintf("/dev/disk/by-uuid/%s", $this->uuid));
		$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
		$cmd->setRedirect2to1();
		$cmd->execute();
	}

	public function addDevice($deviceFile) {
		$cmdArgs = [];
		$cmdArgs[] = "device";
		$cmdArgs[] = "add";
		$cmdArgs[] = escapeshellarg($deviceFile);
		$cmdArgs[] = escapeshellarg(sprintf("/dev/disk/by-uuid/%s", $this->uuid));
		$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
		$cmd->setRedirect2to1();
		$cmd->execute();
	}

	public function removeDevice($deviceFile, $force = FALSE) {
		$cmdArgs = [];
		$cmdArgs[] = "device";
		$cmdArgs[] = "remove";
		if ($force)
			$cmdArgs[] = "--force";
		$cmdArgs[] = escapeshellarg($deviceFile);
		$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
		$cmd->setRedirect2to1();
		$cmd->execute();
	}

	public function evacuateDevice($deviceFile) {
		$cmdArgs = [];
		$cmdArgs[] = "device";
		$cmdArgs[] = "evacuate";
		$cmdArgs[] = escapeshellarg($deviceFile);
		$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
		$cmd->setRedirect2to1();
		$cmd->execute();
	}

	public function getDetails() {
		$cmdArgs = [];
		$cmdArgs[] = "show-super";
		$cmdArgs[] = escapeshellarg($this->getPreferredDeviceFile());
		$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
		$cmd->setRedirect2to1();
		$cmd->execute($output);
		return implode("\n", $output);
	}

	public static function isSubvolume($path) {
		$cmdArgs = [];
		$cmdArgs[] = "subvolume";
		$cmdArgs[] = "show";
		$cmdArgs[] = escapeshellarg($path);
		$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
		$cmd->setRedirect2to1();
		try {
			$cmd->execute($output, $exitStatus);
			return ($exitStatus === 0);
		} catch (\Exception $e) {
			return FALSE;
		}
	}

	public static function createSubvolume($path) {
		$cmdArgs = [];
		$cmdArgs[] = "subvolume";
		$cmdArgs[] = "create";
		$cmdArgs[] = escapeshellarg($path);
		$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
		$cmd->setRedirect2to1();
		$cmd->execute();
	}

	public static function createSnapshot($src, $dest, $readOnly = FALSE) {
		$cmdArgs = [];
		$cmdArgs[] = "subvolume";
		$cmdArgs[] = "snapshot";
		if ($readOnly)
			$cmdArgs[] = "-r";
		$cmdArgs[] = escapeshellarg($src);
		$cmdArgs[] = escapeshellarg($dest);
		$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
		$cmd->setRedirect2to1();
		$cmd->execute();
	}
}

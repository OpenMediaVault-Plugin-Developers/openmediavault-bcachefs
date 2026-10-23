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

	/**
	 * Tell from `bcachefs show-super` output whether the filesystem is
	 * encrypted. Encryption is not a feature bit: the superblock carries a
	 * "crypt" field instead, which show-super lists under "Sections:".
	 * Older tools listed an "encrypt" feature, so accept that too.
	 *
	 * @param string $superText The show-super output.
	 * @return bool
	 */
	public static function isEncryptedSuper(string $superText): bool {
		return 1 === preg_match('/^Sections:.*\bcrypt\b/m', $superText) ||
			1 === preg_match('/^Features:.*\bencrypt\b/m', $superText);
	}

	// On-disk superblock layout (struct bch_sb), see bcachefs_format.h.
	const SB_OFFSET = 4096;
	const SB_MAGIC = "\xc6\x85\x73\xf6\x66\xce\x90\xa9\xd9\x6a\x60\xcf\x80\x3d\xf7\xef";
	const SB_FIXED_SIZE = 752; // up to and including the layout
	const SB_FIELD_CRYPT = 2;

	/**
	 * Parse a raw superblock: the external UUID, the label and the types
	 * of the variable-length fields that follow the fixed part. This needs
	 * no key and works while the filesystem is mounted, unlike show-super
	 * (which wants the passphrase on an encrypted filesystem) and
	 * `unlock -c` (which wants the device exclusively).
	 *
	 * @param string $raw The superblock bytes, starting at its offset.
	 * @return array|NULL ['uuid' => string, 'label' => string,
	 *   'fields' => int[], 'encrypted' => bool], or NULL if this is not a
	 *   complete bcachefs superblock.
	 */
	public static function parseSuperblock(string $raw): ?array {
		if (strlen($raw) < self::SB_FIXED_SIZE ||
				substr($raw, 24, 16) !== self::SB_MAGIC)
			return NULL;
		$u64s = unpack('V', $raw, 124)[1];
		$end = self::SB_FIXED_SIZE + $u64s * 8;
		if (strlen($raw) < $end)
			return NULL;
		$fields = [];
		for ($off = self::SB_FIXED_SIZE; $off + 8 <= $end; ) {
			['u64s' => $fu64s, 'type' => $type] =
				unpack('Vu64s/Vtype', $raw, $off);
			if ($fu64s === 0)
				break;
			$fields[] = $type;
			$off += $fu64s * 8;
		}
		$hex = bin2hex(substr($raw, 56, 16));
		return [
			'uuid'      => vsprintf('%s%s-%s-%s-%s-%s%s%s', str_split($hex, 4)),
			'label'     => rtrim(substr($raw, 72, 32), "\0"),
			'fields'    => $fields,
			'encrypted' => in_array(self::SB_FIELD_CRYPT, $fields, TRUE),
		];
	}

	/**
	 * Find a mounted filesystem's directory under /sys/fs/bcachefs. It is
	 * usually named after the filesystem UUID, but not always (it is not
	 * for an encrypted filesystem), so otherwise look for the directory
	 * that has one of the given devices as a member.
	 *
	 * @param string $uuid The filesystem UUID.
	 * @param array $deviceFiles Member device files, any naming.
	 * @param string $root The sysfs root, for testing.
	 * @return string|NULL The directory, without a trailing slash.
	 */
	public static function findSysfsDir(string $uuid, array $deviceFiles,
			string $root = "/sys/fs/bcachefs"): ?string {
		if ($uuid !== '' && is_dir("{$root}/{$uuid}"))
			return "{$root}/{$uuid}";
		$knames = [];
		foreach ($deviceFiles as $deviceFile) {
			if (!is_string($deviceFile) || $deviceFile === '')
				continue;
			$knames[basename(realpath($deviceFile) ?: $deviceFile)] = TRUE;
		}
		if (empty($knames))
			return NULL;
		foreach (glob("{$root}/*", GLOB_ONLYDIR) ?: [] as $dir) {
			foreach (glob("{$dir}/dev-*", GLOB_ONLYDIR) ?: [] as $devDir) {
				$target = @readlink("{$devDir}/block");
				if (is_string($target) && isset($knames[basename($target)]))
					return $dir;
			}
		}
		return NULL;
	}

	/**
	 * Read and parse the superblock of a member device.
	 *
	 * @param string $deviceFile The device.
	 * @return array|NULL See parseSuperblock().
	 */
	public static function readSuperblock(string $deviceFile): ?array {
		$fh = @fopen($deviceFile, 'rb');
		if ($fh === FALSE)
			return NULL;
		$raw = '';
		if (0 === fseek($fh, self::SB_OFFSET)) {
			$raw = (string)fread($fh, self::SB_FIXED_SIZE);
			if (strlen($raw) === self::SB_FIXED_SIZE) {
				// Fields follow the fixed part; cap at the 1M superblock
				// maximum so a corrupt size cannot run away.
				$rest = min(unpack('V', $raw, 124)[1] * 8, 1 << 20);
				if ($rest > 0)
					$raw .= (string)fread($fh, $rest);
			}
		}
		fclose($fh);
		return self::parseSuperblock($raw);
	}

	/**
	 * Parse the per-device members block of `bcachefs show-super` output
	 * into an array indexed by member number. Fields are only captured
	 * once a "Device N:" header has been seen so the filesystem-level
	 * Label line is not mistaken for a member's.
	 *
	 * @param string $superText The show-super output.
	 * @return array Member number => ['label', 'state', 'size',
	 *   'durability', 'failure_domain', 'lastdevice'], all strings.
	 */
	public static function parseSuperMembers(string $superText): array {
		$fields = [
			'Label'            => 'label',
			'Size'             => 'size',
			'Durability'       => 'durability',
			'State'            => 'state',
			'Failure domain'   => 'failure_domain',
			'Last device name' => 'lastdevice',
		];
		$members = [];
		$idx = NULL;
		foreach (preg_split('/\r?\n/', $superText) as $line) {
			// The device header reads "Device 0:  /dev/...  (model)".
			if (preg_match('/^\s*Device\s+(\d+):/', $line, $m)) {
				$idx = intval($m[1]);
				$members[$idx] = array_fill_keys(array_values($fields), '');
				continue;
			}
			if (is_null($idx))
				continue;
			if (!preg_match('/^\s+([A-Za-z ]+?):\s*(.*?)\s*$/', $line, $m))
				continue;
			if (!isset($fields[$m[1]]))
				continue;
			$value = $m[2];
			if ($fields[$m[1]] === 'label' && $value === '(none)')
				$value = '';
			$members[$idx][$fields[$m[1]]] = $value;
		}
		return $members;
	}

	/**
	 * Read the selected value of a sysfs option. Choice options print
	 * every possible value with the selected one in brackets, e.g.
	 * "[degraded] yes no"; plain options print just the value.
	 *
	 * @param string $value The sysfs file contents.
	 * @return string
	 */
	public static function parseSysfsChoice(string $value): string {
		if (preg_match('/\[([^\]]*)\]/', $value, $m))
			return $m[1];
		return trim($value);
	}

	/**
	 * Convert a bcachefs human-readable size ("512k", "1.00M", "4096")
	 * into bytes. Units are powers of 1024.
	 *
	 * @param string $size The size string.
	 * @return int The size in bytes, or 0 if it cannot be parsed.
	 */
	public static function parseHumanSize(string $size): int {
		if (!preg_match('/^\s*([0-9.]+)\s*([kKmMgGtTpP]?)i?[bB]?\s*$/',
				$size, $m))
			return 0;
		$exp = stripos('kmgtp', $m[2]);
		$mult = ($m[2] === '') ? 1 : 1024 ** ($exp + 1);
		return (int)round(floatval($m[1]) * $mult);
	}

	/**
	 * Whether a member device has grown by at least one bucket beyond
	 * what the filesystem currently uses of it.
	 *
	 * @param int $deviceBytes The block device size.
	 * @param int $nbuckets The number of buckets bcachefs uses.
	 * @param int $bucketBytes The bucket size.
	 * @return bool
	 */
	public static function needsResize(int $deviceBytes, int $nbuckets,
			int $bucketBytes): bool {
		if ($bucketBytes <= 0)
			return FALSE;
		return intdiv($deviceBytes, $bucketBytes) > $nbuckets;
	}

	/**
	 * Parse `bcachefs fs failure-domains --json` output.
	 *
	 * @param string $json The JSON output.
	 * @return array ['separation_violated' => bool, 'offline_devices' =>
	 *   int, 'domains' => [['domain', 'devices', 'lost_bytes',
	 *   'degraded_bytes', 'offline_devices'], ...]], or NULL if the
	 *   output cannot be parsed.
	 */
	public static function parseFailureDomains(string $json): ?array {
		$data = json_decode($json, TRUE);
		if (!is_array($data) || !isset($data['domains']) ||
				!is_array($data['domains']))
			return NULL;
		$domains = [];
		foreach ($data['domains'] as $d) {
			// Named domains carry a device count; a device that is its
			// own domain does not.
			$domains[] = [
				'domain'          => (string)($d['domain'] ?? ''),
				'devices'         => intval($d['devices'] ?? 1),
				'lost_bytes'      => intval($d['lost_bytes'] ?? 0),
				'degraded_bytes'  => intval($d['degraded_bytes'] ?? 0),
				'offline_devices' => intval($d['offline_devices'] ?? 0),
			];
		}
		return [
			'separation_violated' => !empty($data['separation_violated']),
			'offline_devices'     => intval($data['offline_devices'] ?? 0),
			'domains'             => $domains,
		];
	}

	/**
	 * Normalise one entry of `bcachefs subvolume list --json` output. The
	 * layout has changed between bcachefs-tools releases: the path may be
	 * relative to the listed directory or absolute, flags may be a list,
	 * a comma-separated string or an object, a snapshot may be flagged
	 * directly or only carry a snapshot_parent, and usage may be given in
	 * sectors or bytes.
	 *
	 * @param array $sv One decoded JSON entry.
	 * @param string $mnt The mount point that was listed.
	 * @return array ['relpath' => string, 'readonly' => bool,
	 *   'issnapshot' => bool, 'usage' => int|NULL (bytes)], or NULL for
	 *   an entry that names the filesystem root.
	 */
	public static function normalizeSubvolume(array $sv, string $mnt): ?array {
		$mnt = rtrim($mnt, '/');
		$path = trim((string)($sv['path'] ?? $sv['name'] ?? ''));
		if ($mnt !== '' && ($path === $mnt || 0 === strpos($path, $mnt . '/')))
			$path = substr($path, strlen($mnt));
		$path = trim($path, '/');
		if ($path === '')
			return NULL;

		$flags = $sv['flags'] ?? [];
		if (is_string($flags))
			$flags = preg_split('/[\s,|]+/', $flags, -1, PREG_SPLIT_NO_EMPTY);
		if (is_array($flags) && !array_is_list($flags))
			$flags = array_keys(array_filter($flags));
		$flags = array_map('strtolower', (array)$flags);

		$isSnap = !empty($sv['snapshot']) ||
			in_array('snapshot', $flags, TRUE) ||
			!empty($sv['snapshot_parent']) ||
			str_starts_with($path, '.snapshots/');
		$isRO = !empty($sv['ro']) || !empty($sv['readonly']) ||
			in_array('ro', $flags, TRUE) ||
			in_array('readonly', $flags, TRUE) ||
			in_array('read_only', $flags, TRUE);

		$usage = NULL;
		if (isset($sv['sectors']) && is_numeric($sv['sectors'])) {
			$usage = intval($sv['sectors']) * 512;
		} else {
			foreach (['usage', 'disk_usage', 'size', 'bytes'] as $key) {
				if (isset($sv[$key]) && is_numeric($sv[$key])) {
					$usage = intval($sv[$key]);
					break;
				}
			}
		}
		return [
			'relpath'    => $path,
			'readonly'   => $isRO,
			'issnapshot' => $isSnap,
			'usage'      => $usage,
		];
	}

	/**
	 * List the subvolumes (and optionally snapshots) below a mount point.
	 *
	 * @param string $mnt The mount point.
	 * @param bool $snapshots Include snapshots.
	 * @return array|NULL A list of normalizeSubvolume() entries, or NULL
	 *   if the running kernel/tools cannot list subvolumes.
	 */
	public static function listSubvolumes(string $mnt, bool $snapshots = TRUE): ?array {
		$cmdArgs = ["subvolume", "list", "--json", "--recursive"];
		if ($snapshots)
			$cmdArgs[] = "--snapshots";
		$cmdArgs[] = escapeshellarg($mnt);
		$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
		$cmd->setQuiet(TRUE);
		try {
			$cmd->execute($output);
		} catch (\Exception $e) {
			return NULL;
		}
		$data = json_decode(implode("", $output), TRUE);
		if (!is_array($data))
			return NULL;
		$result = [];
		foreach ($data as $sv) {
			if (!is_array($sv))
				continue;
			if (!is_null($entry = self::normalizeSubvolume($sv, $mnt)))
				$result[] = $entry;
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

	/**
	 * Scrub every member device. Given a single device path, bcachefs
	 * scrubs only that member, so always pass the mount point.
	 */
	public function scrub() {
		$this->assertIsMounted();
		$cmdArgs = [];
		$cmdArgs[] = "scrub";
		$cmdArgs[] = escapeshellarg($this->getMountPoint());
		$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
		$cmd->setRedirect2to1();
		$cmd->execute();
	}

	public function addDevice($deviceFile) {
		$this->assertIsMounted();
		$cmdArgs = [];
		$cmdArgs[] = "device";
		$cmdArgs[] = "add";
		$cmdArgs[] = escapeshellarg($this->getMountPoint());
		$cmdArgs[] = escapeshellarg($deviceFile);
		$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
		$cmd->setRedirect2to1();
		$cmd->execute();
	}

	/**
	 * Grow the filesystem onto the free space of any member device that
	 * has been enlarged. Members that are already fully used are left
	 * alone, so this is safe to run at any time.
	 *
	 * @return void
	 * @throw \OMV\ExecException
	 */
	public function grow() {
		$this->assertIsMounted();
		$sysfs = self::findSysfsDir($this->getUuid(), $this->getDeviceFiles());
		if (is_null($sysfs)) {
			throw new \OMV\Exception(
				"Cannot find the running filesystem '%s' in sysfs.",
				$this->getUuid());
		}
		foreach (glob("{$sysfs}/dev-*", GLOB_ONLYDIR) ?: [] as $devDir) {
			$target = @readlink("{$devDir}/block");
			if (!is_string($target))
				continue;
			$kname = basename($target);
			$sectors = @file_get_contents("/sys/class/block/{$kname}/size");
			$nbuckets = @file_get_contents("{$devDir}/nbuckets");
			$bucketSize = @file_get_contents("{$devDir}/bucket_size");
			if (!is_string($sectors) || !is_string($nbuckets) ||
					!is_string($bucketSize))
				continue;
			if (!self::needsResize(intval($sectors) * 512, intval($nbuckets),
					self::parseHumanSize(trim($bucketSize))))
				continue;
			$cmdArgs = [];
			$cmdArgs[] = "device";
			$cmdArgs[] = "resize";
			$cmdArgs[] = escapeshellarg("/dev/" . $kname);
			$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
			$cmd->setRedirect2to1();
			$cmd->execute();
		}
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

	/**
	 * Show the superblock. On an encrypted filesystem show-super needs the
	 * key, which a system service cannot see in the keyring it was unlocked
	 * into, so it prints nothing; show the usage report instead.
	 */
	public function getDetails() {
		$output = [];
		exec(sprintf("/usr/sbin/bcachefs show-super %s </dev/null 2>&1",
			escapeshellarg($this->getPreferredDeviceFile())), $output, $rc);
		if (preg_grep('/^External UUID:/', $output))
			return implode("\n", $output);
		if ($this->isMounted()) {
			$usage = [];
			exec(sprintf("/usr/sbin/bcachefs fs usage -h %s 2>&1",
				escapeshellarg($this->getMountPoint())), $usage);
			return implode("\n", array_merge([
				gettext("The superblock of an encrypted filesystem cannot be shown without its passphrase (show-super: ") .
					trim(implode(" ", $output)) . ").",
				"",
			], $usage));
		}
		throw new \OMV\Exception("Failed to read the superblock of '%s': %s",
			$this->getPreferredDeviceFile(), implode("\n", $output));
	}

	/**
	 * Check whether a path is the root of a subvolume or snapshot.
	 *
	 * @param string $path The absolute path.
	 * @return bool
	 */
	public static function isSubvolume($path) {
		$real = realpath($path);
		if ($real === FALSE)
			return FALSE;
		$mnt = self::findMountPoint($real);
		if (is_null($mnt) || $real === $mnt)
			return FALSE;
		foreach (self::listSubvolumes($mnt) ?? [] as $sv) {
			if ($mnt . '/' . $sv['relpath'] === $real)
				return TRUE;
		}
		return FALSE;
	}

	/**
	 * Find the bcachefs mount point that contains a path, preferring the
	 * longest (most specific) match.
	 *
	 * @param string $path An absolute, resolved path.
	 * @return string|NULL
	 */
	public static function findMountPoint(string $path): ?string {
		$best = NULL;
		foreach (file("/proc/mounts", FILE_IGNORE_NEW_LINES |
				FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
			$parts = preg_split('/\s+/', $line);
			if (!isset($parts[2]) || $parts[2] !== "bcachefs")
				continue;
			$mnt = rtrim(unescape_path($parts[1]), '/');
			if ($path !== $mnt && 0 !== strpos($path, $mnt . '/'))
				continue;
			if (is_null($best) || strlen($mnt) > strlen($best))
				$best = $mnt;
		}
		return $best;
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

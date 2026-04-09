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
namespace OMV\System\Filesystem\Backend;

class Bcachefs extends BackendAbstract {
	public function __construct() {
		$this->type = "bcachefs";
		$this->properties = self::PROP_MNTENT | self::PROP_POSIX_ACL |
			self::PROP_DISCARD | self::PROP_COMPRESS |
			self::PROP_RESIZE;
		$this->mkfsOptions = "-f";
		$this->mntOptions = ["defaults", "nofail"];
	}

	/**
	 * Bcachefs RAID reports multiple devices per filesystem (one per member
	 * device). We deduplicate by UUID and resolve the canonical device path
	 * via /dev/disk/by-uuid/<UUID>, falling back to findfs when the symlink
	 * does not exist.
	 */
	public function enumerateByBlkid(array $enums) {
		$result = [];

		// Canonicalize all device paths first so that e.g. /dev/mapper/xxx
		// entries can be matched later.
		foreach ($enums as $enumk => &$enumv) {
			$enumv['devicefile'] = realpath($enumv['devicefile']);
		}
		unset($enumv);

		$uniqueEnums = array_unique_key($enums, "uuid");
		foreach ($uniqueEnums as $uniqueEnumv) {
			$canonicalDeviceFile = realpath(sprintf(
				"/dev/disk/by-uuid/%s", $uniqueEnumv['uuid']));
			$filtered = array_filter_ex($enums, "devicefile",
				$canonicalDeviceFile);
			if (empty($filtered)) {
				// Symlink may not exist yet; use findfs as a fallback.
				$cmdArgs = [];
				$cmdArgs[] = sprintf("UUID=%s", $uniqueEnumv['uuid']);
				$cmd = new \OMV\System\Process("findfs", $cmdArgs);
				$cmd->setRedirect2to1();
				$filtered[] = array_merge($uniqueEnumv, [
					"devicefile" => $cmd->execute()
				]);
			}
			$result[] = $filtered[0];
		}
		return $result;
	}

	public function getFstabMntOptions(
	  \OMV\System\Storage\StorageDevice $sd = null) {
		$options = parent::getFstabMntOptions($sd);
		if (!is_null($sd)) {
			if (!$sd->isRotational())
				$options[] = "ssd";
		}
		return $options;
	}

	function getImpl($args) {
		$object = new \OMV\System\Filesystem\Bcachefs($args);
		$object->setBackend($this);
		return $object;
	}
}

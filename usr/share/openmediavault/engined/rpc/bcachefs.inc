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
		$this->registerMethod("createFilesystem");
	}

    public function createFilesystem($params, $context)
    {
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		$this->validateMethodParams($params, "rpc.bcachefs.createfilesystem");

		return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
		  use ($params) {
			$deviceFiles = explode(",", $params['devicefiles']);

			// Wipe existing data on every target device.
			foreach ($deviceFiles as $deviceFile) {
				$sd = \OMV\System\Storage\StorageDevice::assertGetStorageDevice(
					$deviceFile);
				$sd->wipe();
			}

			// Build bcachefs format command.
			$cmdArgs = [];
			$cmdArgs[] = "format";
			$cmdArgs[] = "--force";

			if (!empty($params['label'])) {
				$cmdArgs[] = "--label";
				$cmdArgs[] = escapeshellarg($params['label']);
			}
			if (!empty($params['replicas'])) {
				$cmdArgs[] = sprintf("--replicas=%d",
					intval($params['replicas']));
			}
			if (!empty($params['compression']) &&
			  $params['compression'] !== "none") {
				$cmdArgs[] = sprintf("--compression=%s",
					escapeshellarg($params['compression']));
			}

			// Devices must come last.
			foreach ($deviceFiles as $deviceFile) {
				$cmdArgs[] = escapeshellarg($deviceFile);
			}

			$cmd = new \OMV\System\Process("bcachefs", $cmdArgs);
			$cmd->setRedirect2to1();
			if (0 !== ($exitStatus = $this->exec($cmd, $output,
			  $bgOutputFilename))) {
				throw new \OMV\ExecException($cmd, $output, $exitStatus);
			}
		});
	}
}

<?php
/**
 * test-parsers.php — Unit tests for the bcachefs output parsers.
 *
 * Usage: php tests/test-parsers.php
 *
 * Needs no root, devices or openmediavault install: the parent class is
 * stubbed so the filesystem backend can be loaded on its own.
 */
namespace OMV\System\Filesystem {
	if (!class_exists(Filesystem::class)) {
		class Filesystem {}
	}
}

namespace {
	require_once __DIR__ .
		'/../usr/share/php/openmediavault/system/filesystem/bcachefs.inc';

	use OMV\System\Filesystem\Bcachefs;

	$pass = 0;
	$fail = 0;

	function check(string $desc, $expected, $actual): void {
		global $pass, $fail;
		if ($expected === $actual) {
			echo "  PASS  $desc\n";
			$pass++;
		} else {
			echo "  FAIL  $desc\n";
			echo "        expected: " . var_export($expected, true) . "\n";
			echo "        actual:   " . var_export($actual, true) . "\n";
			$fail++;
		}
	}

	echo "=== parseUsageDevices ===\n";

	// Summary table, all members present.
	$out = [
		"Filesystem: 5e9e2f5c-7f0b-4a7e-9d4a-0e1b2c3d4e5f",
		"Size:                       1.80 TiB",
		"",
		"Device label           Device      State     Size      Used  Use%",
		"(no label) (device 0): dm-18       rw     931 GiB   120 GiB   12%",
		"ssd.ssd1 (device 1):   sda         rw     931 GiB   118 GiB   12%",
	];
	check("summary table: both members online",
		[
			['idx' => 0, 'dev' => 'dm-18', 'online' => true],
			['idx' => 1, 'dev' => 'sda',   'online' => true],
		],
		Bcachefs::parseUsageDevices($out));

	// 1.39.3+: a listed but absent member reads as offline, named by its
	// sysfs directory since it has no block device.
	$out = [
		"Device label           Device      State     Size      Used  Use%",
		"hdd.hdd1 (device 0):   sdb         rw     3.6 TiB   1.2 TiB   33%",
		"hdd.hdd2 (device 2):   dev-2       offline      -         -     -",
	];
	$members = Bcachefs::parseUsageDevices($out);
	check("summary table: offline member is reported",
		[
			['idx' => 0, 'dev' => 'sdb',   'online' => true],
			['idx' => 2, 'dev' => 'dev-2', 'online' => false],
		],
		$members);
	check("summary table: offline member has no device file",
		['sdb'],
		array_values(array_map(fn($m) => $m['dev'],
			array_filter($members, fn($m) => $m['online']))));

	// Full per-device breakdown (bcachefs fs usage -f devices).
	$out = [
		"hdd.hdd1 (device 0):\tsdb\trw\t33%",
		"  data type\tbuckets\tsectors",
		"  btree:\t1024\t2097152",
		"hdd.hdd2 (device 1):\tdev-1\toffline\tusage unavailable",
	];
	check("full breakdown: offline member is reported",
		[
			['idx' => 0, 'dev' => 'sdb',   'online' => true],
			['idx' => 1, 'dev' => 'dev-1', 'online' => false],
		],
		Bcachefs::parseUsageDevices($out));

	// Older tools: device name only, state column may be absent.
	check("older output without a state column",
		[['idx' => 0, 'dev' => 'sdc', 'online' => true]],
		Bcachefs::parseUsageDevices(["(no label) (device 0): sdc"]));

	check("no member lines", [], Bcachefs::parseUsageDevices(["", "Size: 1 GiB"]));

	echo "\n=== parseDamageList ===\n";

	$out = [
		"videos/holiday.mkv: ptr_to_missing_device extent_csum_bad",
		"docs/report.pdf: btree_node_read_error",
		"photos/raw",
		"",
	];
	check("files with and without an error list",
		[
			['path' => 'videos/holiday.mkv',
			 'errors' => ['ptr_to_missing_device', 'extent_csum_bad'],
			 'line' => 'videos/holiday.mkv: ptr_to_missing_device extent_csum_bad'],
			['path' => 'docs/report.pdf',
			 'errors' => ['btree_node_read_error'],
			 'line' => 'docs/report.pdf: btree_node_read_error'],
			['path' => 'photos/raw', 'errors' => [], 'line' => 'photos/raw'],
		],
		Bcachefs::parseDamageList($out));

	check("': ' inside a file name stays part of the path",
		[['path' => 'notes: Q3 draft.txt', 'errors' => ['extent_csum_bad'],
		  'line' => 'notes: Q3 draft.txt: extent_csum_bad']],
		Bcachefs::parseDamageList(["notes: Q3 draft.txt: extent_csum_bad"]));

	check("name with ': ' and upper case, no errors, is kept whole",
		[['path' => 'Meeting: Notes', 'errors' => [], 'line' => 'Meeting: Notes']],
		Bcachefs::parseDamageList(["Meeting: Notes"]));

	check("spaces in path",
		[['path' => 'My Videos/a b.mkv', 'errors' => ['stripe_reconstruct_failed'],
		  'line' => 'My Videos/a b.mkv: stripe_reconstruct_failed']],
		Bcachefs::parseDamageList(["My Videos/a b.mkv: stripe_reconstruct_failed"]));

	check("trailing CR/LF is stripped",
		[['path' => 'a', 'errors' => ['x_y'], 'line' => 'a: x_y']],
		Bcachefs::parseDamageList(["a: x_y\r\n"]));

	check("empty output", [], Bcachefs::parseDamageList([""]));

	echo "\n$pass passed, $fail failed\n";
	exit($fail === 0 ? 0 : 1);
}

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

namespace OMV\System\Filesystem\Backend {
	if (!class_exists(BackendAbstract::class)) {
		// Mirrors what the real base class does with mount options.
		class BackendAbstract {
			public const PROP_MNTENT = 0x1;
			public const PROP_POSIX_ACL = 0x2;
			public const PROP_DISCARD = 0x4;
			public const PROP_RESIZE = 0x10;
			public const PROP_COMPRESS = 0x40;
			protected $type;
			protected $properties;
			protected $mkfsOptions;
			protected $mntOptions = [];
			public function getFstabMntOptions($sd = null) {
				return $this->mntOptions;
			}
		}
	}
}

namespace {
	require_once __DIR__ .
		'/../usr/share/php/openmediavault/system/filesystem/bcachefs.inc';
	require_once __DIR__ .
		'/../usr/share/php/openmediavault/system/filesystem/backend/bcachefs.inc';

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

	echo "\n=== isEncryptedSuper ===\n";

	// Captured from bcachefs-tools 1.39.7: encryption shows up only as the
	// "crypt" superblock section, never in Features.
	$encrypted = implode("\n", [
		"External UUID:                             287e5bd4-f8db-4881-b99e-dbbcef8258ad",
		"Label:                                     (none)",
		"Devices:                                   1",
		"Sections:                                  members_v1,crypt,members_v2,ext",
		"Features:                                  new_siphash,new_extent_overwrite,btree_ptr_v2,extents_above_btree_updates,btree_updates_journalled,new_varint,journal_no_flush,alloc_v2,extents_across_btree_nodes,incompat_version_field",
	]);
	$plain = implode("\n", [
		"Label:                                     my fs",
		"Sections:                                  replicas_v0,disk_groups,clean,journal_v2,counters,members_v2,ext,downgrade,extent_type_u64s,errors_v2",
		"Features:                                  new_siphash,new_extent_overwrite,btree_ptr_v2,extents_above_btree_updates,btree_updates_journalled,new_varint,journal_no_flush,alloc_v2,extents_across_btree_nodes,incompat_version_field",
	]);
	check("crypt section means encrypted", true, Bcachefs::isEncryptedSuper($encrypted));
	check("no crypt section means not encrypted", false, Bcachefs::isEncryptedSuper($plain));
	check("legacy encrypt feature still detected", true,
		Bcachefs::isEncryptedSuper("Features:  lz4,encrypt,reflink"));
	check("'crypt' inside another word does not count", false,
		Bcachefs::isEncryptedSuper("Sections:  members_v2,cryptfoo"));
	check("a label containing 'crypt' does not count", false,
		Bcachefs::isEncryptedSuper("Label:  crypt\nSections:  members_v2"));

	echo "\n=== parseSuperblock / readSuperblock ===\n";

	// Raw superblocks written by bcachefs-tools 1.39.7 `format`.
	$fixtures = __DIR__ . '/fixtures';
	$enc = Bcachefs::parseSuperblock(
		file_get_contents("$fixtures/superblock-encrypted.bin"));
	$plain = Bcachefs::parseSuperblock(
		file_get_contents("$fixtures/superblock-plain.bin"));
	check("encrypted: crypt field found", true, $enc['encrypted']);
	check("encrypted: label", "enc", $enc['label']);
	check("encrypted: external UUID", "23b7480d-8670-4edd-83df-48bbf4385f1c",
		$enc['uuid']);
	check("encrypted: field types", [13, 11, 2, 1], $enc['fields']);
	check("plain: no crypt field", false, $plain['encrypted']);
	check("plain: label", "plain", $plain['label']);
	check("plain: external UUID", "998ca1f3-9c32-429e-a405-8ca5d2c8d081",
		$plain['uuid']);
	check("not a superblock", null,
		Bcachefs::parseSuperblock(str_repeat("\0", 1024)));
	check("truncated superblock", null,
		Bcachefs::parseSuperblock(substr(
			file_get_contents("$fixtures/superblock-plain.bin"), 0, 1000)));
	check("too short", null, Bcachefs::parseSuperblock("abc"));

	// readSuperblock() reads from the 4K superblock offset of a device.
	$img = tempnam(sys_get_temp_dir(), 'bcachefs-sb-');
	file_put_contents($img, str_repeat("\0", 4096) .
		file_get_contents("$fixtures/superblock-encrypted.bin") .
		str_repeat("\0", 4096));
	check("readSuperblock: device image", true,
		Bcachefs::readSuperblock($img)['encrypted'] ?? null);
	unlink($img);
	check("readSuperblock: missing device", null,
		Bcachefs::readSuperblock("/nonexistent/device"));

	echo "\n=== findSysfsDir ===\n";

	// Fake /sys/fs/bcachefs: one filesystem named by its UUID, and one (as
	// for an encrypted filesystem) named otherwise, whose member is dm-9.
	$tmp = sys_get_temp_dir() . '/bcachefs-sysfs-' . getmypid();
	$root = "$tmp/sys";
	mkdir("$root/aaaaaaaa-0000-0000-0000-000000000000/dev-0", 0777, true);
	mkdir("$root/internal-name/dev-0", 0777, true);
	mkdir("$root/internal-name/dev-2", 0777, true);
	mkdir("$tmp/dev/mapper", 0777, true);
	symlink("../../../devices/virtual/block/sdb",
		"$root/aaaaaaaa-0000-0000-0000-000000000000/dev-0/block");
	symlink("../../../devices/virtual/block/sdc", "$root/internal-name/dev-0/block");
	symlink("../../../devices/virtual/block/dm-9", "$root/internal-name/dev-2/block");
	touch("$tmp/dev/dm-9");
	symlink("../dm-9", "$tmp/dev/mapper/vg-lv");   // like /dev/mapper/*

	check("directory named by UUID", "$root/aaaaaaaa-0000-0000-0000-000000000000",
		Bcachefs::findSysfsDir("aaaaaaaa-0000-0000-0000-000000000000", [], $root));
	check("directory found by member device (through a symlink)",
		"$root/internal-name",
		Bcachefs::findSysfsDir("bbbbbbbb-0000-0000-0000-000000000000",
			["$tmp/dev/mapper/vg-lv"], $root));
	check("no matching member", null,
		Bcachefs::findSysfsDir("bbbbbbbb-0000-0000-0000-000000000000",
			["/dev/nonexistent-sdz"], $root));
	check("no UUID directory and no devices", null,
		Bcachefs::findSysfsDir("bbbbbbbb-0000-0000-0000-000000000000", [], $root));
	exec("rm -rf " . escapeshellarg($tmp));

	echo "\n=== parseSuperMembers ===\n";

	$super = implode("\n", [
		"Label:                                     tank",
		"Options:",
		"  data_replicas:                           2",
		"Device 0:                                  /dev/sdb (WDC)",
		"  Label:                                   ssd.ssd1",
		"  Failure domain:                          shelf1",
		"  Size:                                    1.00G",
		"  State:                                   rw",
		"  Durability:                              1",
		"  Last device name:                        /dev/sdb",
		"Device 2:                                  (not found)\t",
		"  Label:                                   (none)",
		"  Size:                                    2.00G",
		"  State:                                   ro",
		"  Durability:                              2",
		"  Last device name:                        sdd",
	]);
	check("members keyed by slot, gap kept, fs label ignored",
		[
			0 => ['label' => 'ssd.ssd1', 'size' => '1.00G', 'durability' => '1',
				'state' => 'rw', 'failure_domain' => 'shelf1',
				'lastdevice' => '/dev/sdb'],
			2 => ['label' => '', 'size' => '2.00G', 'durability' => '2',
				'state' => 'ro', 'failure_domain' => '',
				'lastdevice' => 'sdd'],
		],
		Bcachefs::parseSuperMembers($super));
	check("no members", [], Bcachefs::parseSuperMembers("Label: x\nDevices: 0"));

	echo "\n=== parseSysfsChoice ===\n";

	check("choice option", "degraded", Bcachefs::parseSysfsChoice("[degraded] yes no \n"));
	check("choice in the middle", "fix_safe",
		Bcachefs::parseSysfsChoice("continue [fix_safe] panic ro "));
	check("plain value", "zstd", Bcachefs::parseSysfsChoice("zstd\n"));
	check("number", "45", Bcachefs::parseSysfsChoice("45\n"));
	check("empty", "", Bcachefs::parseSysfsChoice(""));

	echo "\n=== parseHumanSize ===\n";

	check("512k", 524288, Bcachefs::parseHumanSize("512k"));
	check("1.00M", 1048576, Bcachefs::parseHumanSize("1.00M"));
	check("2G", 2147483648, Bcachefs::parseHumanSize("2G"));
	check("plain bytes", 4096, Bcachefs::parseHumanSize("4096"));
	check("KiB suffix", 1024, Bcachefs::parseHumanSize("1KiB"));
	check("garbage", 0, Bcachefs::parseHumanSize("lots"));

	echo "\n=== needsResize ===\n";

	// Live values: 3272 buckets of 512k on a 3350528-sector device.
	check("device fully used", false,
		Bcachefs::needsResize(3350528 * 512, 3272, 524288));
	check("less than one bucket spare", false,
		Bcachefs::needsResize(3350528 * 512 + 524287, 3272, 524288));
	check("exactly one bucket spare", true,
		Bcachefs::needsResize(3350528 * 512 + 524288, 3272, 524288));
	check("device grown by 1G", true,
		Bcachefs::needsResize(3350528 * 512 + 1073741824, 3272, 524288));
	check("unknown bucket size", false, Bcachefs::needsResize(1 << 30, 10, 0));

	echo "\n=== parseFailureDomains ===\n";

	// Captured from bcachefs-tools 1.39.7 on a two-device filesystem.
	$fd = '{"data_bytes":16252928,"devices":2,"domains":[' .
		'{"degraded_bytes":16252928,"domain":"dm-18","lost_bytes":0,"offline_devices":0},' .
		'{"degraded_bytes":16252928,"domain":"dm-19","lost_bytes":0,"offline_devices":0}],' .
		'"filesystem":"29ea73c7-26b6-4b0f-ac22-cfedc2a5ad9f","offline_devices":0,' .
		'"separation_violated":false}';
	check("devices as their own domains",
		[
			'separation_violated' => false,
			'offline_devices' => 0,
			'domains' => [
				['domain' => 'dm-18', 'devices' => 1, 'lost_bytes' => 0,
				 'degraded_bytes' => 16252928, 'offline_devices' => 0],
				['domain' => 'dm-19', 'devices' => 1, 'lost_bytes' => 0,
				 'degraded_bytes' => 16252928, 'offline_devices' => 0],
			],
		],
		Bcachefs::parseFailureDomains($fd));
	$fd = '{"devices":3,"domains":[{"domain":"shelf1","devices":2,' .
		'"lost_bytes":1024,"degraded_bytes":0,"offline_devices":1}],' .
		'"offline_devices":1,"separation_violated":true}';
	check("named domain with a violation",
		[
			'separation_violated' => true,
			'offline_devices' => 1,
			'domains' => [
				['domain' => 'shelf1', 'devices' => 2, 'lost_bytes' => 1024,
				 'degraded_bytes' => 0, 'offline_devices' => 1],
			],
		],
		Bcachefs::parseFailureDomains($fd));
	check("not JSON", null, Bcachefs::parseFailureDomains("error: no such fs"));
	check("JSON without domains", null, Bcachefs::parseFailureDomains('{"x":1}'));

	echo "\n=== normalizeSubvolume ===\n";

	$mnt = "/srv/dev-disk-by-uuid-x";
	check("relative path, flags list, sectors",
		['relpath' => 'data', 'readonly' => true, 'issnapshot' => false,
		 'usage' => 1024],
		Bcachefs::normalizeSubvolume(
			['path' => 'data', 'flags' => ['ro'], 'sectors' => 2,
			 'snapshot_parent' => 0], $mnt));
	check("absolute path under the mount point",
		['relpath' => 'a/b', 'readonly' => false, 'issnapshot' => false,
		 'usage' => null],
		Bcachefs::normalizeSubvolume(['path' => "$mnt/a/b"], $mnt));
	check("snapshot via snapshot_parent, flags string",
		['relpath' => 'snaps/s1', 'readonly' => true, 'issnapshot' => true,
		 'usage' => null],
		Bcachefs::normalizeSubvolume(
			['path' => '/snaps/s1', 'flags' => 'ro,unlinked',
			 'snapshot_parent' => 7], $mnt));
	check("snapshot via flags object, bytes usage",
		['relpath' => 'x', 'readonly' => false, 'issnapshot' => true,
		 'usage' => 4096],
		Bcachefs::normalizeSubvolume(
			['name' => 'x', 'flags' => ['snapshot' => true, 'ro' => false],
			 'usage' => 4096], $mnt));
	check("snapshot by .snapshots location",
		['relpath' => '.snapshots/data/test-20260101-000000', 'readonly' => false,
		 'issnapshot' => true, 'usage' => null],
		Bcachefs::normalizeSubvolume(
			['path' => '.snapshots/data/test-20260101-000000'], $mnt));
	check("readonly as a boolean key",
		['relpath' => 'y', 'readonly' => true, 'issnapshot' => false,
		 'usage' => null],
		Bcachefs::normalizeSubvolume(['path' => 'y', 'readonly' => true], $mnt));
	check("filesystem root is skipped", null,
		Bcachefs::normalizeSubvolume(['path' => '/'], $mnt));
	check("mount point itself is skipped", null,
		Bcachefs::normalizeSubvolume(['path' => $mnt], $mnt));
	check("sibling mount with the same prefix is not stripped",
		'srv/dev-disk-by-uuid-xy/z',
		Bcachefs::normalizeSubvolume(['path' => '/srv/dev-disk-by-uuid-xy/z'],
			$mnt)['relpath']);

	echo "\n=== backend mount options ===\n";

	$backend = new \OMV\System\Filesystem\Backend\Bcachefs();
	check("no btrfs 'ssd' option", ['defaults', 'nofail'],
		$backend->getFstabMntOptions(null));
	check("still no 'ssd' when given a device", false,
		in_array('ssd', $backend->getFstabMntOptions(new \stdClass()), true));

	echo "\n$pass passed, $fail failed\n";
	exit($fail === 0 ? 0 : 1);
}

<?php

namespace Engined\Module;

class Bcachefs extends \OMV\Engine\Module\ServiceAbstract {
	public function getName() {
		return "bcachefs";
	}

	public function deployAfter(): array {
		return ["apt"];
	}
}

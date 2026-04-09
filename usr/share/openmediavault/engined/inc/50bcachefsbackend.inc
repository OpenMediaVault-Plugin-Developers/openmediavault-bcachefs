<?php

$mngr = \OMV\System\Filesystem\Backend\Manager::getInstance();
$mngr->registerBackend(new \OMV\System\Filesystem\Backend\Bcachefs());

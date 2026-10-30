<?php
define ('SS_IMPORTCRYPTO', 108<<8);
class hooks_import_crypto extends hooks {

	function __construct() {
		$this->module_name = 'import_crypto';
	}

	/*
		Install additional menu options provided by module
	*/
	function install_options($app) {
		global $path_to_root;

		switch($app->id) {
			case 'GL':
				$app->add_rapp_function(2, _('Import Crypto Transactions'), 
					$path_to_root.'/modules/'.$this->module_name.'/import_crypto.php', 'SS_IMPORTCRYPTO');
		}
	}

	function install_access()
	{
		$security_sections[SS_IMPORTCRYPTO] =	_("Import Crypto Transactions");

		$security_areas['SA_CSVIMPORT'] = array(SS_IMPORTCRYPTO|108, _("Import Crypto Transactions"));

		return array($security_areas, $security_sections);
	}
}

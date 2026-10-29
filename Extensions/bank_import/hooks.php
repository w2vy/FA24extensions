<?php
define ('SS_BANKIMPORT', 109<<8);
class hooks_bank_import extends hooks {
	var $module_name = 'Bank Statement Import';

	/*
		Install additonal menu options provided by module
	*/
	function install_options($app) {
		global $path_to_root;

		switch($app->id) {
			case 'GL':
				$app->add_rapp_function(2, _('Bank Statement Import'),
					$path_to_root.'/modules/bank_import/bank_import.php', 'SA_BANKIMPORT');
		}
	}

	function install_access()
	{
		$security_sections[SS_BANKIMPORT] =	_("Bank Statement Import");

		$security_areas['SA_BANKIMPORT'] = array(SS_BANKIMPORT|109, _("Bank Statement Import"));

		return array($security_areas, $security_sections);
	}
}

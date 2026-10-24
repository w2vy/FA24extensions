<?php
define ('SS_MEDUSAORDERS', 108<<8);
class hooks_medusa_orders extends hooks {
	var $module_name = 'Medusa Shop Interface';

	/*
		Install additonal menu options provided by module
	*/
	function install_options($app) {
		global $path_to_root;

		switch($app->id) {
			case 'orders':
				$app->add_rapp_function(2, _('Medusa Shop (GotGrit)'),
					$path_to_root.'/modules/medusa_orders/medusa.php', 'SA_MEDUSAORDERS');
		}
	}

	function install_access()
	{
		$security_sections[SS_MEDUSAORDERS] =	_("Medusa Shop Interface");

		$security_areas['SA_MEDUSAORDERS'] = array(SS_MEDUSAORDERS|108, _("Medusa Shop Interface"));

		return array($security_areas, $security_sections);
	}
}

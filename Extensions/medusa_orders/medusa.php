<?php
/**********************************************
Author: Tom Moulton
Name: Medusa shop (GotGrit) order import to Sales Order
Free software under GNU GPLv3
***********************************************/
$page_security = 'SA_MEDUSAORDERS';
$path_to_root="../..";

include($path_to_root . "/includes/session.inc");
add_access_extensions();

include_once($path_to_root . "/includes/ui.inc");
include_once($path_to_root . "/includes/db/crm_contacts_db.inc");
include_once($path_to_root . "/includes/data_checks.inc");
include_once($path_to_root . "/sales/includes/db/branches_db.inc");
include_once($path_to_root . "/sales/includes/db/customers_db.inc");
include_once($path_to_root . "/sales/includes/db/sales_order_db.inc");
include_once($path_to_root . "/sales/includes/db/payment_db.inc");
include_once($path_to_root . "/sales/includes/cart_class.inc");
include_once($path_to_root . "/sales/includes/ui/sales_order_ui.inc");
include_once($path_to_root . "/inventory/includes/db/items_prices_db.inc");
include_once($path_to_root . "/modules/medusa_orders/medusa_lib.inc");

check_db_has_sales_areas("You must first define atleast one Sales Area");

$found = medusa_table_exists();
$action = isset($_GET['action']) ? $_GET['action'] : 'summary';
if (isset($_POST['action'])) $action = $_POST['action'];
if (!$found && $action != 'create') $action = 'show';

$SETTINGS = array('url', 'apikey', 'stripekey', 'startorder', 'taxgroup', 'bank');

if ($action == 'create') {
    medusa_create_table();
    medusa_set('startorder', '0');
    meta_forward($_SERVER['PHP_SELF'], "action=show");
}

if ($action == 'update') {
    foreach ($SETTINGS as $k) {
        // Keys are never echoed back; an empty key field keeps the saved one.
        if (in_array($k, array('apikey', 'stripekey')) && get_post($k) == '') continue;
        medusa_set($k, trim(get_post($k)));
    }
    display_notification("Settings saved");
    $action = 'show';
}

$import_log = array();
if ($action == 'o_import') {
    if (!check_num('credit_limit', 0)) {
        display_error(_("The credit limit must be numeric and not less than zero."));
        $action = 'oimport';
    } else {
        $error = '';
        $orders = medusa_orders_to_import(medusa_get('startorder', 0), $error);
        if ($error != '') display_error($error);
        $opts = array(
            'currency' => get_post('currency'), 'payment_terms' => get_post('payment_terms'),
            'credit_limit' => input_num('credit_limit'), 'sales_type' => get_post('sales_type'),
            'salesman' => get_post('salesman'), 'area' => get_post('area'),
            'sales_account' => get_post('sales_account'), 'sales_discount_account' => get_post('sales_discount_account'),
            'receivables_account' => get_post('receivables_account'),
            'payment_discount_account' => get_post('payment_discount_account'),
            'default_location' => get_post('default_location'), 'tax_group_id' => get_post('tax_group_id'),
            'bank_account' => get_post('bank_account'));
        foreach ($orders as $o) $import_log[] = medusa_import_order($o, $opts);
        if (!count($orders) && $error == '') $import_log[] = "No new orders to import.";
        $action = 'oimport';
    }
}

page("Medusa Shop Interface");
$tabs = array('summary' => 'Summary', 'show' => 'Configuration', 'oimport' => 'Order Import');
$sep = '';
foreach ($tabs as $a => $label) {
    echo $sep;
    if ($action == $a) echo $label;
    else hyperlink_params($_SERVER['PHP_SELF'], _($label), "action=$a", false);
    $sep = '&nbsp;|&nbsp;';
}
echo "<br><br>";

if ($action == 'summary') {
    $error = '';
    $orders = $found ? medusa_orders_to_import(medusa_get('startorder', 0), $error) : array();
    if ($error != '') display_error($error);
    start_table(TABLESTYLE);
    table_header(array("Shop order", "Date", "Email", "Total", "Status"));
    $k = 0;
    foreach ($orders as $o) {
        alt_table_row_color($k);
        label_cell('#' . $o['display_id']);
        label_cell(substr($o['created_at'], 0, 10));
        label_cell($o['email']);
        amount_cell($o['total']);
        label_cell($o['payment_status'] . ' / ' . $o['fulfillment_status']);
        end_row();
    }
    if (!count($orders)) label_row("", "No orders waiting (paid AND fulfilled, after #" . medusa_get('startorder', 0) . ")", "colspan=4");
    end_table(1);
    end_page();
}

if ($action == 'show') {
    start_form(true);
    start_table(TABLESTYLE);
    table_header(array("Setting", "Value"));
    label_row("Table Status", $found ? "Found" : "<font color=red>Not Found</font>");
    if ($found) {
        text_row("Shop URL (https://…)", 'url', medusa_get('url'), 40, 200);
        text_row("Medusa secret API key" . (medusa_get('apikey') != '' ? " (saved; blank = keep)" : ""), 'apikey', '', 40, 200);
        text_row("Stripe read key" . (medusa_get('stripekey') != '' ? " (saved; blank = keep)" : ""), 'stripekey', '', 40, 200);
        text_row("Last imported shop order #", 'startorder', medusa_get('startorder', 0), 10, 20);
        tax_groups_list_row(_("Tax group outside Florida:"), 'taxgroup', medusa_get('taxgroup'));
        bank_accounts_list_row(_("Bank account for payments:"), 'bank', medusa_get('bank'));
    }
    end_table(1);
    if (!$found) {
        hidden('action', 'create');
        submit_center('create', 'Create Table');
    } else {
        hidden('action', 'update');
        submit_center('update', 'Save');
    }
    end_form();
    end_page();
}

if ($action == 'oimport') {
    foreach ($import_log as $line) display_notification($line);

    start_form(true);
    start_table(TABLESTYLE2, "width=40%");
    table_section_title("Default GL Accounts (new customers)");
    $company_record = get_company_prefs();
    if (!isset($_POST['sales_account']) || $_POST['sales_account'] == "")
        $_POST['sales_account'] = $company_record["default_sales_act"];
    if (!isset($_POST['sales_discount_account']) || $_POST['sales_discount_account'] == "")
        $_POST['sales_discount_account'] = $company_record["default_sales_discount_act"];
    if (!isset($_POST['receivables_account']) || $_POST['receivables_account'] == "")
        $_POST['receivables_account'] = $company_record["debtors_act"];
    if (!isset($_POST['payment_discount_account']) || $_POST['payment_discount_account'] == "")
        $_POST['payment_discount_account'] = $company_record["default_prompt_payment_act"];
    gl_all_accounts_list_row("Sales Account:", 'sales_account', $_POST['sales_account']);
    gl_all_accounts_list_row("Sales Discount Account:", 'sales_discount_account', $_POST['sales_discount_account']);
    gl_all_accounts_list_row("Receivables Account:", 'receivables_account', $_POST['receivables_account']);
    gl_all_accounts_list_row("Payment Discount Account:", 'payment_discount_account', $_POST['payment_discount_account']);

    global $SysPrefs;
    table_section_title("Location, Tax, Prices, Payments");
    locations_list_row("Location:", 'default_location', null);
    tax_groups_list_row(_("Tax group outside Florida:"), 'tax_group_id', medusa_get('taxgroup'));
    sales_types_list_row("List prices from Sales Type:", 'sales_type', null);
    sales_persons_list_row("Sales Person:", 'salesman', null);
    sales_areas_list_row("Sales Area:", 'area');
    currencies_list_row("Customer Currency:", 'currency', get_company_pref("curr_default"));
    payment_terms_list_row("Payment Terms:", 'payment_terms', null);
    amount_row(_("Credit Limit:"), 'credit_limit', price_format($SysPrefs->default_credit_limit()));
    bank_accounts_list_row(_("Bank account for payments:"), 'bank_account', medusa_get('bank'));
    end_table(1);

    hidden('action', 'o_import');
    submit_center('oimport', "Import Shop Orders");
    end_form();
    end_page();
}

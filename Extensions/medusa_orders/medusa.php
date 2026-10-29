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
include_once($path_to_root . "/gl/includes/gl_db.inc");
include_once($path_to_root . "/includes/ui/items_cart.inc");
include_once($path_to_root . "/modules/medusa_orders/medusa_lib.inc");
include_once($path_to_root . "/modules/medusa_orders/medusa_push.inc");
include_once($path_to_root . "/modules/medusa_orders/medusa_dispatch.inc");

check_db_has_sales_areas("You must first define atleast one Sales Area");

$found = medusa_table_exists();
$action = isset($_GET['action']) ? $_GET['action'] : 'summary';
if (isset($_POST['action'])) $action = $_POST['action'];
if (!$found && $action != 'create') $action = 'show';
if ($found) medusa_upgrade_table();

$SETTINGS = array('url', 'fallback_url', 'apikey', 'stripekey', 'startorder', 'taxgroup', 'bank', 'location', 'dispatchfrom', 'cartons');

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

// The timer (w2vy cron, medusa_sync.sh): import, push, then dispatch, as plain text.
// Dispatch last: it moves stock within the pools and off undelivered orders by the
// same amount, so the push sends the same numbers either way, and a dispatch that
// dies mid-run can't hold the push back.
// "ERROR: " lines are mailed; "DONE" last means the run finished.
if ($action == 'cron') {
    header('Content-Type: text/plain; charset=utf-8');
    $opts = medusa_import_opts();
    if (!$opts) {
        echo "ERROR: no import options saved yet: run one Order Import by hand first\n";
    } else {
        list($lines, $errors) = medusa_run_import($opts);
        foreach ($lines as $l) echo "$l\n";
        foreach ($errors as $e) echo "ERROR: $e\n";
        list($lines, $errors) = medusa_run_push();
        foreach ($lines as $l) echo "$l\n";
        foreach ($errors as $e) echo "ERROR: $e\n";
        list($lines, $errors) = medusa_run_dispatch();
        foreach ($lines as $l) echo "$l\n";
        foreach ($errors as $e) echo "ERROR: $e\n";
    }
    global $MEDUSA_ROUTE;
    if ($MEDUSA_ROUTE) echo "ROUTE: ".$MEDUSA_ROUTE[0]." failed (".$MEDUSA_ROUTE[1]."); used ".$MEDUSA_ROUTE[2]."\n";
    echo "DONE\n";
    exit;
}

$import_log = array();
$import_errors = array();
if ($action == 'o_import') {
    if (!check_num('credit_limit', 0)) {
        display_error(_("The credit limit must be numeric and not less than zero."));
        $action = 'oimport';
    } else {
        $opts = array();
        foreach ($MEDUSA_OPT_KEYS as $k) $opts[$k] = $k == 'credit_limit' ? input_num($k) : get_post($k);
        // The timer imports with the options of the last manual import.
        medusa_set('import_opts', json_encode($opts));
        list($import_log, $import_errors) = medusa_run_import($opts);
        $action = 'oimport';
    }
}

$push_log = array();
$push_errors = array();
if ($action == 'p_push') {
    list($push_log, $push_errors) = medusa_run_push();
    $action = 'push';
}

$dispatch_log = array();
$dispatch_errors = array();
if ($action == 'd_dispatch') {
    list($dispatch_log, $dispatch_errors) = medusa_run_dispatch();
    $action = 'dispatch';
}

page("Medusa Shop Interface");
$tabs = array('summary' => 'Summary', 'show' => 'Configuration', 'oimport' => 'Order Import',
    'push' => 'Price + Stock Push', 'dispatch' => 'Dispatch');
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
        text_row("Shop URL (https://...)", 'url', medusa_get('url'), 40, 200);
        text_row("Fallback URL when the shop URL fails (Flux address, blank = none)", 'fallback_url', medusa_get('fallback_url'), 40, 200);
        text_row("Medusa secret API key" . (medusa_get('apikey') != '' ? " (saved; blank = keep)" : ""), 'apikey', '', 40, 200);
        text_row("Stripe read key" . (medusa_get('stripekey') != '' ? " (saved; blank = keep)" : ""), 'stripekey', '', 40, 200);
        text_row("Last imported shop order #", 'startorder', medusa_get('startorder', 0), 10, 20);
        tax_groups_list_row(_("Tax group outside Florida:"), 'taxgroup', medusa_get('taxgroup'));
        bank_accounts_list_row(_("Bank account for payments:"), 'bank', medusa_get('bank'));
        locations_list_row(_("Shop stock location:"), 'location', medusa_get('location', 'DEF'));
        text_row("Auto-dispatch from sales order # (blank = off)", 'dispatchfrom', medusa_get('dispatchfrom'), 10, 20);
        textarea_row("Sealed cartons that open into bulk<br>(one per line: AA509 = WAO9 * 44)", 'cartons', medusa_get('cartons'), 30, 6);
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
    foreach ($import_errors as $line) display_error($line);
    foreach ($import_log as $line) display_notification($line);
    // Start from the options the timer uses (the last manual import).
    $saved = medusa_import_opts();
    if ($saved && !isset($_POST['oimport']))
        foreach ($saved as $k => $v) if (!isset($_POST[$k])) $_POST[$k] = $k == 'credit_limit' ? price_format($v) : $v;

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
    if (!isset($_POST['tax_group_id'])) $_POST['tax_group_id'] = medusa_get('taxgroup');
    tax_groups_list_row(_("Tax group outside Florida:"), 'tax_group_id', null);
    sales_types_list_row("List prices from Sales Type:", 'sales_type', null);
    sales_persons_list_row("Sales Person:", 'salesman', null);
    sales_areas_list_row("Sales Area:", 'area');
    currencies_list_row("Customer Currency:", 'currency', get_company_pref("curr_default"));
    payment_terms_list_row("Payment Terms:", 'payment_terms', null);
    amount_row(_("Credit Limit:"), 'credit_limit', price_format($SysPrefs->default_credit_limit()));
    if (!isset($_POST['bank_account'])) $_POST['bank_account'] = medusa_get('bank');
    bank_accounts_list_row(_("Bank account for payments:"), 'bank_account', null);
    end_table(1);

    hidden('action', 'o_import');
    submit_center('oimport', "Import Shop Orders");
    end_form();
    end_page();
}

if ($action == 'push') {
    foreach ($push_errors as $line) display_error($line);
    foreach ($push_log as $line) display_notification($line);
    $error = '';
    list($rows, $variants, $links, $levels, $error) = medusa_push_plan(medusa_get('location', 'DEF'));
    if ($error != '') display_error($error);
    display_note("Changes the push would make now (FA is the master; stock in shop units: oz, half kg, each).", 0, 1);
    start_table(TABLESTYLE);
    table_header(array("Change", "SKU", "Shop now", "From FA", "Note"));
    $k = 0;
    foreach ($rows as $r) {
        alt_table_row_color($k);
        foreach ($r as $c) label_cell($c);
        end_row();
    }
    if (!count($rows) && $error == '') label_row("", "Nothing to change: the shop matches FA.", "colspan=4");
    end_table(1);
    start_form(true);
    hidden('action', 'p_push');
    submit_center('p_push', "Push to Shop");
    end_form();
    end_page();
}

if ($action == 'dispatch') {
    foreach ($dispatch_errors as $line) display_error($line);
    foreach ($dispatch_log as $line) display_notification($line);
    $from = medusa_dispatch_from();
    if (!$from) display_note("Auto-dispatch is off: set \"Auto-dispatch from sales order #\" under Configuration.", 0, 1);
    else display_note("Imported orders from sales order $from on, not yet delivered, oldest first. The timer dispatches them every run.", 0, 1);
    start_table(TABLESTYLE);
    table_header(array("Sales order", "Date", "Shop order", "Will open", "Will make", "Short (held)"));
    $k = 0;
    $avail = array();
    $orders = $from ? medusa_orders_to_dispatch($from) : array();
    foreach ($orders as $so) {
        list($wos, $short, $opens) = medusa_dispatch_plan($so, $avail);
        $make = array();
        foreach ($wos as $w) $make[$w[0]] = (isset($make[$w[0]]) ? $make[$w[0]] : 0) + $w[1];
        alt_table_row_color($k);
        label_cell(get_trans_view_str(ST_SALESORDER, $so['order_no']));
        label_cell(sql2date($so['ord_date']));
        label_cell($so['customer_ref']);
        label_cell(count($short) ? '' : medusa_opens_text($opens));
        label_cell(medusa_qty_text($make));
        label_cell(count($short) ? "<font color=red>".medusa_qty_text($short)."</font>" : '');
        end_row();
    }
    if ($from && !count($orders)) label_row("", "Nothing waiting.", "colspan=4");
    end_table(1);
    if ($from && count($orders)) {
        start_form(true);
        hidden('action', 'd_dispatch');
        submit_center('d_dispatch', "Dispatch Now");
        end_form();
    }
    end_page();
}

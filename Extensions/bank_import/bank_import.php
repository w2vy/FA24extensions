<?php
/**********************************************
Author: Tom Moulton
Name: Bank statement import (Suncoast CSV, PayPal CSV) -> payments, deposits, reconcile
Free software under GNU GPLv3
***********************************************/
$page_security = 'SA_BANKIMPORT';
$path_to_root="../..";

include($path_to_root . "/includes/session.inc");
add_access_extensions();

include_once($path_to_root . "/includes/ui.inc");
include_once($path_to_root . "/includes/ui/items_cart.inc");
include_once($path_to_root . "/includes/db/crm_contacts_db.inc");
include_once($path_to_root . "/gl/includes/gl_db.inc");
include_once($path_to_root . "/sales/includes/sales_db.inc");
include_once($path_to_root . "/purchasing/includes/purchasing_db.inc");
include_once($path_to_root . "/admin/db/fiscalyears_db.inc");
include_once($path_to_root . "/modules/bank_import/bank_import_lib.inc");

$action = isset($_GET['action']) ? $_GET['action'] : 'upload';
if (isset($_POST['action'])) $action = $_POST['action'];
bi_create_tables();     // IF NOT EXISTS: also adds tables a later version brought (settings)

/** FA banks with a Stripe key: id => name. */
function bi_stripe_banks() {
    $out = array();
    $res = db_query("SELECT id, bank_account_name FROM ".TB_PREF."bank_accounts WHERE inactive = 0 ORDER BY bank_account_name", "banks");
    while ($b = db_fetch_assoc($res)) if (bi_get('stripe_key:'.$b['id']) != '') $out[$b['id']] = $b['bank_account_name'];
    return $out;
}

$ACTIONS = array('skip' => 'Skip (nothing in FA)', 'gl' => 'GL account #', 'supplier' => 'Supplier #',
    'customer' => 'Customer #');

function bi_action_select($name, $selected) {
    global $ACTIONS;
    $opts = array('leave' => 'Leave for later');
    $res = db_query("SELECT id, description FROM ".TB_PREF."quick_entries WHERE type IN (".QE_PAYMENT.", ".QE_DEPOSIT.")"
        ." ORDER BY description", "quick entries");
    while ($q = db_fetch_assoc($res)) $opts['qe:'.$q['id']] = 'QE: '.$q['description'];
    $opts += $ACTIONS;
    $opts += bi_transfer_options();
    $h = "<select name='$name'>";
    foreach ($opts as $k => $v)
        $h .= "<option value='".htmlspecialchars($k, ENT_QUOTES)."'".($k == $selected ? ' selected' : '').">"
            .htmlspecialchars($v)."</option>";
    return $h."</select>";
}

/** One "Transfer with <bank name>" choice per FA bank account (value transfer:<id>). */
function bi_transfer_options() {
    $opts = array();
    $res = db_query("SELECT id, bank_account_name FROM ".TB_PREF."bank_accounts WHERE inactive = 0 ORDER BY bank_account_name", "banks");
    while ($b = db_fetch_assoc($res)) $opts['transfer:'.$b['id']] = 'Transfer with '.$b['bank_account_name'];
    return $opts;
}

/** The select value for an action + target (qe and transfer carry their target in the value). */
function bi_action_value($action, $target) {
    return ($action == 'qe' || $action == 'transfer') && $target != '' ? "$action:$target" : $action;
}

function bi_bt_text($rows) {
    $out = array();
    foreach ($rows as $b) $out[] = get_trans_view_str($b['type'], $b['trans_no'], $b['type'].'/'.$b['trans_no'])
        .' '.sql2date($b['trans_date']).' '.number_format($b['amount'], 2);
    return implode('<br>', $out);
}

// ---- upload -> preview -----------------------------------------------------

$log = array();
$errors = array();
if ($action == 'preview') {
    $bank = (int)get_post('bank_account');
    if (!isset($_FILES['imp']) || $_FILES['imp']['name'] == '') {
        $errors[] = "No CSV file selected";
        $action = 'upload';
    } else {
        list($lines, $err) = bi_parse($_FILES['imp']['tmp_name']);
        if ($err != '') {
            $errors[] = $err;
            $action = 'upload';
        } else
            $_SESSION['bank_import'] = array('bank' => $bank, 'file' => $_FILES['imp']['name'],
                'upload_id' => time(), 'lines' => bi_plan($bank, $lines));
    }
}
if ($action == 'fetch') {
    $bank = (int)get_post('stripe_bank');
    list($lines, $err) = bi_stripe_lines($bank);
    if ($err != '') {
        $errors[] = $err;
        $action = 'upload';
    } else {
        $_SESSION['bank_import'] = array('bank' => $bank, 'file' => 'Stripe',
            'upload_id' => time(), 'lines' => bi_plan($bank, $lines));
        $action = 'preview';
    }
}

// ---- Stripe settings ----------------------------------------------------------

$ST_FIELDS = array('gl' => 'Income GL account #', 'to' => 'Payouts land in (FA bank #)',
    'from' => 'Fetch from (Y-m-d, blank = all)', 'skip' => 'Payouts already booked by hand (po_..., comma separated)');
if ($action == 'stripe_save') {
    $bank = (int)get_post('st_bank');
    if (trim(get_post('st_key')) != '') bi_set("stripe_key:$bank", trim(get_post('st_key')));
    if (check_value('st_clear')) bi_set("stripe_key:$bank", '');
    foreach ($ST_FIELDS as $f => $label) bi_set("stripe_$f:$bank", trim(get_post("st_$f")));
    if (bi_get("stripe_gl:$bank") != '' && !get_gl_account(bi_get("stripe_gl:$bank")))
        $errors[] = "no GL account ".bi_get("stripe_gl:$bank");
    if (bi_get("stripe_to:$bank") != '' && !get_bank_account(bi_get("stripe_to:$bank")))
        $errors[] = "no bank account #".bi_get("stripe_to:$bank");
    if (!count($errors)) $log[] = "Stripe settings saved for ".bi_bank_name($bank);
    $action = 'stripe';
}

if ($action == 'create' && isset($_SESSION['bank_import'])) {
    $S = $_SESSION['bank_import'];
    $bank = $S['bank'];
    $todo = array();
    foreach ($S['lines'] as $i => $l) {
        $p = $l['plan'];
        if (in_array($p['status'], array('rule', 'unknown'))) {
            $act = get_post("act_$i", 'leave');
            $tgt = trim(get_post("tgt_$i"));
            if (preg_match('/^(qe|transfer):(\d+)$/', $act, $m)) {
                $act = $m[1];
                $tgt = $m[2];
            }
            if ($act == 'leave') continue;
            $e = bi_check($bank, $l, $act, $tgt);
            if ($e != '') $errors[] = sql2date($l['posted'])." ".$l['descr']." ".number_format($l['amount'], 2).": $e";
            $todo[$i] = array($act, $tgt);
        } elseif ($p['status'] != 'loaded')
            $todo[$i] = array($p['status'], '');
    }
    if (!count($errors)) {
        begin_transaction();
        $n = array();
        foreach ($todo as $i => $t) {
            $l = $S['lines'][$i];
            list($act, $tgt) = $t;
            if ($act == 'matched' || $act == 'group') {
                $ids = array();
                foreach ($l['plan']['bt'] as $b) $ids[] = $b['id'];
                bi_save_line($bank, $l, $act, $ids, $S['upload_id']);
            } elseif ($act == 'elsewhere')
                bi_save_line($bank, $l, 'elsewhere', array(), $S['upload_id']);
            else {
                $bt = bi_post($bank, $l, $act, $tgt);
                bi_save_line($bank, $l, $act == 'skip' ? 'skipped' : 'created', array($bt), $S['upload_id']);
                if (check_value("rem_$i") && trim(get_post("pat_$i")) != '')
                    bi_add_rule(50, $bank, trim(get_post("pat_$i")), $l['amount'] > 0 ? 1 : -1, $act, $tgt);
            }
            $n[$act] = (isset($n[$act]) ? $n[$act] : 0) + 1;
        }
        commit_transaction();
        unset($_SESSION['bank_import']);
        foreach ($n as $k => $v) $log[] = "$v line(s): $k";
        if (!count($n)) $log[] = "Nothing to do.";
        $action = 'upload';
    } else
        $action = 'preview';
}

// ---- rules ------------------------------------------------------------------

if ($action == 'rule_add') {
    if (trim(get_post('pattern')) == '' || get_post('raction') == '')
        $errors[] = "A rule needs a pattern and an action";
    else {
        $ract = get_post('raction');
        $rtgt = trim(get_post('target'));
        if (preg_match('/^transfer:(\d+)$/', $ract, $m)) {
            $ract = 'transfer';
            $rtgt = $m[1];
        }
        bi_add_rule(get_post('priority', 50), get_post('rbank', 0), trim(get_post('pattern')), get_post('sign', 0), $ract, $rtgt);
    }
    $action = 'rules';
}
if ($action == 'rule_del') {
    db_query("DELETE FROM ".TB_PREF."bank_import_rules WHERE id = ".(int)$_GET['id'], "could not delete the rule");
    $action = 'rules';
}
if ($action == 'rule_seed') {
    foreach (bi_starter_rules() as $r) call_user_func_array('bi_add_rule', $r);
    $log[] = count(bi_starter_rules())." starter rules added";
    $action = 'rules';
}

// ---- reconcile --------------------------------------------------------------

if ($action == 'reconcile' && isset($_POST['r_reconcile'])) {
    $bank = (int)get_post('bank_account');
    $to = date2sql(get_post('reconcile_date'));
    list($rl, $fa_rec, $open, $end, $fa_after, $problems) = bi_reconcile_plan($bank, $to);
    if (count($problems)) $errors = $problems;
    else {
        bi_reconcile($bank, $to, $rl, $end);
        $log[] = count($rl)." statement line(s) reconciled at ".sql2date($to).", balance ".number_format($end, 2);
        unset($_POST['reconcile_date']);    // on to the next month
    }
}

// ---- page -------------------------------------------------------------------

$js = '';
if (user_use_date_picker()) $js .= get_js_date_picker();
page("Bank Statement Import", false, false, '', $js);
$tabs = array('upload' => 'Upload', 'rules' => 'Rules', 'reconcile' => 'Reconcile', 'stripe' => 'Stripe');
$sep = '';
foreach ($tabs as $a => $label) {
    echo $sep;
    if ($action == $a || ($a == 'upload' && $action == 'preview')) echo $label;
    else hyperlink_params($_SERVER['PHP_SELF'], _($label), "action=$a", false);
    $sep = '&nbsp;|&nbsp;';
}
echo "<br><br>";
foreach ($errors as $e) display_error($e);
foreach ($log as $e) display_notification($e);

if ($action == 'upload') {
    start_form(true);
    start_table(TABLESTYLE2, "width=50%");
    table_section_title("Statement CSV (Suncoast download, or PayPal \"Balance affecting\")");
    bank_accounts_list_row("Bank Account:", 'bank_account', null);
    label_row('CSV file:', "<input type='file' id='imp' name='imp'>");
    end_table(1);
    hidden('action', 'preview');
    submit_center('preview', "Preview");
    end_form();
    if (count($sb = bi_stripe_banks())) {
        start_form();
        start_table(TABLESTYLE2, "width=50%");
        table_section_title("Or fetch a Stripe account's transactions");
        $o = '';
        foreach ($sb as $id => $name) $o .= "<option value='$id'>".htmlspecialchars($name)."</option>";
        label_row("Bank Account:", "<select name='stripe_bank'>$o</select>");
        end_table(1);
        hidden('action', 'fetch');
        submit_center('fetch', "Fetch from Stripe");
        end_form();
    }
    end_page();
}

if ($action == 'stripe') {
    $bank = (int)get_post('st_bank', ($sb = bi_stripe_banks()) ? key($sb) : 0);
    display_note("A Stripe account is a clearing bank account: Fetch (Upload tab) loads its balance transactions."
        ." Payments go to the income GL (fee to bank charges), refunds back to it, payouts = transfer to the payout bank."
        ." Use a read-only key.", 0, 1);
    // Picking the bank is its own form: a change must never post (and save) the settings below.
    start_form();
    start_table(TABLESTYLE2);
    bank_accounts_list_row("Bank Account:", 'st_bank', $bank);
    end_table(1);
    hidden('action', 'stripe');
    submit_center('show', "Show", true, false, false);
    end_form();
    start_form();
    hidden('st_bank', $bank);
    start_table(TABLESTYLE2);
    table_section_title(bi_bank_name($bank));
    $k = bi_get("stripe_key:$bank");
    text_row("Stripe key".($k != '' ? " (saved: ".substr($k, 0, 8)."…; blank = keep)" : ""), 'st_key', '', 40, 200);
    if ($k != '') check_row("Remove the saved key:", 'st_clear', 0);
    foreach ($ST_FIELDS as $f => $label) {
        $_POST["st_$f"] = bi_get("stripe_$f:$bank");
        text_row("$label:", "st_$f", null, 40, 255);
    }
    end_table(1);
    hidden('action', 'stripe_save');
    submit_center('stripe_save', "Save");
    end_form();
    end_page();
}

if ($action == 'preview') {
    $S = $_SESSION['bank_import'];
    $count = array();
    foreach ($S['lines'] as $l) $count[$l['plan']['status']] = (isset($count[$l['plan']['status']]) ? $count[$l['plan']['status']] : 0) + 1;
    $txt = array();
    foreach ($count as $k => $v) $txt[] = "$v $k";
    display_note($S['file']." into ".get_bank_account($S['bank'])['bank_account_name'].": ".count($S['lines'])
        ." line(s), ".implode(', ', $txt).". Nothing is posted until you press Create.", 0, 1);
    start_form();
    start_table(TABLESTYLE);
    table_header(array("Posted", "Description", "Amount", "Balance", "Found", "Action", "#", "Remember as rule"));
    $k = 0;
    foreach ($S['lines'] as $i => $l) {
        $p = $l['plan'];
        alt_table_row_color($k);
        label_cell(sql2date($l['posted']), "nowrap");
        label_cell(htmlspecialchars($l['descr']));
        amount_cell($l['amount']);
        amount_cell($l['balance']);
        if ($p['status'] == 'matched' || $p['status'] == 'group') {
            label_cell("in FA: ".bi_bt_text($p['bt']).($p['note'] != '' ? "<br><i>".$p['note']."</i>" : ''));
            label_cell(''); label_cell(''); label_cell('');
        } elseif ($p['status'] == 'loaded' || $p['status'] == 'elsewhere') {
            label_cell("<i>".htmlspecialchars($p['note'])."</i>");
            label_cell(''); label_cell(''); label_cell('');
        } else {
            label_cell(($p['status'] == 'rule' ? "<b>will create</b>" : "<font color=red>unknown</font>")
                .($p['note'] != '' ? "<br><i>".htmlspecialchars($p['note'])."</i>" : ''));
            $sel = get_post("act_$i", bi_action_value($p['action'], $p['target']));
            label_cell(bi_action_select("act_$i", $sel));
            label_cell("<input type='text' name='tgt_$i' size=6 value='"
                .htmlspecialchars(get_post("tgt_$i", in_array($p['action'], array('qe', 'transfer')) ? '' : $p['target']), ENT_QUOTES)."'>");
            label_cell($p['status'] == 'unknown' ? "<input type='checkbox' name='rem_$i' value=1>"
                ." <input type='text' name='pat_$i' size=18 value='"
                .htmlspecialchars(get_post("pat_$i", bi_pattern_guess($l['descr'])), ENT_QUOTES)."'>" : '');
        }
        end_row();
    }
    end_table(1);
    hidden('action', 'create');
    submit_center('create', "Create");
    end_form();
    end_page();
}

if ($action == 'rules') {
    $res = db_query("SELECT * FROM ".TB_PREF."bank_import_rules ORDER BY bank_act, priority, id", "rules");
    display_note("Rules are tried lowest priority number first; the first match wins. Payout = a deposit that"
        ." customer payments add up to. Sign: 1 money in, -1 money out, 0 either.", 0, 1);
    start_table(TABLESTYLE);
    table_header(array("Priority", "Bank", "Pattern", "Sign", "Action", "#", ""));
    $k = 0;
    $n = 0;
    while ($r = db_fetch_assoc($res)) {
        $n++;
        alt_table_row_color($k);
        label_cell($r['priority']);
        label_cell($r['bank_act'] ? bi_bank_name($r['bank_act']) : 'any');
        label_cell(htmlspecialchars($r['pattern']));
        label_cell($r['sign']);
        label_cell($r['action']);
        if ($r['action'] == 'transfer') label_cell(bi_bank_name($r['target']));
        else label_cell($r['target'].($r['action'] == 'qe' && ($q = get_quick_entry($r['target'])) ? ' '.$q['description'] : ''));
        label_cell("<a href='".$_SERVER['PHP_SELF']."?action=rule_del&id=".$r['id']."'>Delete</a>");
        end_row();
    }
    end_table(1);
    if (!$n) {
        start_form();
        hidden('action', 'rule_seed');
        submit_center('rule_seed', "Load starter rules");
        end_form();
    }
    start_form();
    start_table(TABLESTYLE2);
    table_section_title("Add a rule");
    text_row("Priority:", 'priority', 50, 5, 5);
    bank_accounts_list_row("Bank Account:", 'rbank', null);
    text_row("Pattern (in the description):", 'pattern', '', 30, 100);
    text_row("Sign (1 in, -1 out, 0 either):", 'sign', 0, 3, 3);
    label_row("Action:", "<select name='raction'><option value='qe'>quick entry</option><option value='gl'>GL account</option>"
        ."<option value='supplier'>supplier</option><option value='customer'>customer</option>"
        ."<option value='payout'>payout</option><option value='skip'>skip</option>"
        .implode('', array_map(function ($k, $v) { return "<option value='$k'>".htmlspecialchars($v)."</option>"; },
            array_keys(bi_transfer_options()), bi_transfer_options()))."</select>");
    text_row("# (quick entry, GL, supplier, customer):", 'target', '', 10, 20);
    end_table(1);
    hidden('action', 'rule_add');
    submit_center('rule_add', "Add rule");
    end_form();
    end_page();
}

if ($action == 'reconcile') {
    // Default = the bank of the last upload (PayPal after a PayPal file), else 8.
    $last = db_fetch_row(db_query("SELECT bank_act FROM ".TB_PREF."bank_import_lines ORDER BY id DESC LIMIT 1", "last upload"));
    $bank = (int)get_post('bank_account', $last ? $last[0] : 8);
    if (get_post('reconcile_date') == '') {
        // Default = the end of the first month not yet reconciled (or the last line, if sooner): statements go month by month.
        $r = db_fetch_row(db_query("SELECT MIN(posted), MAX(posted) FROM ".TB_PREF."bank_import_lines WHERE reconciled IS NULL AND bank_act = "
            .db_escape($bank), "last line"));
        $_POST['reconcile_date'] = $r && $r[0] ? sql2date(min(date('Y-m-t', strtotime($r[0])), $r[1])) : Today();
    }
    $to = date2sql($_POST['reconcile_date']);
    start_form();
    start_table(TABLESTYLE2);
    bank_accounts_list_row("Bank Account:", 'bank_account', $bank);
    date_row("Statement date (reconcile up to):", 'reconcile_date');
    end_table(1);
    hidden('action', 'reconcile');
    submit_center('show', "Show", true, false, false);
    list($rl, $fa_rec, $open, $end, $fa_after, $problems) = bi_reconcile_plan($bank, $to);
    start_table(TABLESTYLE);
    table_header(array("FA reconciled", "Statement opens", "Statement ends", "FA after", "Lines"));
    start_row();
    amount_cell($fa_rec); amount_cell($open); amount_cell($end); amount_cell($fa_after); label_cell(count($rl));
    end_row();
    end_table(1);
    if (!count($rl)) display_note("Every loaded line up to ".sql2date($to)." is reconciled. Upload the next statement to go on.", 0, 1);
    else {
        foreach ($problems as $p) display_warning($p);
        if (!count($problems)) submit_center('r_reconcile', "Reconcile", true, false, false);
    }
    end_form();
    $fa = bi_fa_not_at_bank($bank, $to);
    if (count($fa)) {
        display_note("In FA, not on a loaded statement line (stay unreconciled):", 0, 1);
        start_table(TABLESTYLE);
        table_header(array("FA", "Date", "Amount", "Person"));
        $k = 0;
        foreach ($fa as $b) {
            alt_table_row_color($k);
            label_cell(get_trans_view_str($b['type'], $b['trans_no'], $b['type'].'/'.$b['trans_no']));
            label_cell(sql2date($b['trans_date']));
            amount_cell($b['amount']);
            label_cell(payment_person_name($b['person_type_id'], $b['person_id']));
            end_row();
        }
        end_table(1);
    }
    end_page();
}

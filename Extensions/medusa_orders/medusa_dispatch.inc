<?php
/**********************************************
Author: Tom Moulton
Name: Medusa shop interface: auto-dispatch of imported orders
Free software under GNU GPLv3

The import takes only paid AND shipped orders, so FA just catches up. Per
order, oldest first, all or nothing: what is not on hand is made through the
BOMs (nested: kit -> jars -> bulk + jar, deepest first, as assembly work
orders), then the whole order is delivered (today), invoiced dated the order
date, and the order's payment(s) allocated to the invoice. Short anywhere in
the tree = the order is held (nothing posted) and retried next run.

Only imported orders (cust_ref "Medusa Order # N" / "Woo Order # N") from the
"dispatch from" sales order on; blank = off.
***********************************************/

include_once($path_to_root . "/sales/includes/sales_db.inc");
include_once($path_to_root . "/sales/includes/db/custalloc_db.inc");
include_once($path_to_root . "/manufacturing/includes/manufacturing_db.inc");
include_once($path_to_root . "/admin/db/fiscalyears_db.inc");

/** First sales order number to dispatch; 0 = off. */
function medusa_dispatch_from() {
    $v = trim(medusa_get('dispatchfrom'));
    return $v === '' ? 0 : (int)$v;
}

/** Imported sales orders from $from on with anything left to deliver, oldest first. */
function medusa_orders_to_dispatch($from) {
    global $MEDUSA_REF, $MEDUSA_WOO_REF;
    $out = array();
    $res = db_query("SELECT o.order_no, o.ord_date, o.customer_ref, o.debtor_no, o.branch_code, o.from_stk_loc"
        ." FROM ".TB_PREF."sales_orders o WHERE o.trans_type = ".ST_SALESORDER." AND o.order_no >= ".(int)$from
        ." AND (o.customer_ref LIKE ".db_escape($MEDUSA_REF.'%')." OR o.customer_ref LIKE ".db_escape($MEDUSA_WOO_REF.'%').")"
        ." AND EXISTS (SELECT 1 FROM ".TB_PREF."sales_order_details d WHERE d.order_no = o.order_no"
        ." AND d.trans_type = o.trans_type AND d.quantity > d.qty_sent)"
        ." ORDER BY o.ord_date, o.order_no", "orders to dispatch");
    while ($r = db_fetch_assoc($res)) $out[] = $r;
    return $out;
}

/** mb_flag of an FA item, or null if there is no such item. */
function medusa_mb_flag($sku) {
    static $flags = null;
    if ($flags === null) {
        $flags = array();
        $res = db_query("SELECT stock_id, mb_flag FROM ".TB_PREF."stock_master", "item types");
        while ($r = db_fetch_row($res)) $flags[$r[0]] = $r[1];
    }
    return isset($flags[$sku]) ? $flags[$sku] : null;
}

/** BOM of an FA item: [[component, qty per unit, location]]. */
function medusa_bom($sku) {
    static $bom = null;
    if ($bom === null) {
        $bom = array();
        $res = db_query("SELECT parent, component, quantity, loc_code FROM ".TB_PREF."bom ORDER BY parent, id", "bom");
        while ($r = db_fetch_assoc($res)) $bom[$r['parent']][] = array($r['component'], (float)$r['quantity'], $r['loc_code']);
    }
    return isset($bom[$sku]) ? $bom[$sku] : array();
}

function medusa_qoh($sku, $loc) {
    $row = db_fetch_row(db_query("SELECT SUM(qty) FROM ".TB_PREF."stock_moves WHERE stock_id = ".db_escape($sku)
        ." AND loc_code = ".db_escape($loc), "on hand"));
    return round((float)$row[0], 6);
}

/**
 * Takes $qty of $sku at $loc from the simulated stock $avail; the rest is made
 * through the BOM. Work orders go to $wos deepest first; what can't be had or
 * made goes to $short.
 */
function medusa_need($sku, $qty, $loc, &$avail, &$wos, &$short, $depth = 0) {
    $flag = medusa_mb_flag($sku);
    if ($flag == 'D') return;   // service item: no stock
    $k = "$loc|$sku";
    if (!isset($avail[$k])) $avail[$k] = $flag === null ? 0.0 : max(0.0, medusa_qoh($sku, $loc));
    $take = min($avail[$k], $qty);
    $avail[$k] = round($avail[$k] - $take, 6);
    $rest = round($qty - $take, 6);
    if ($rest <= 0) return;
    $bom = medusa_bom($sku);
    if ($flag != 'M' || !count($bom) || $depth > 10) {
        $short[$sku] = (isset($short[$sku]) ? $short[$sku] : 0) + $rest;
        return;
    }
    foreach ($bom as $c) medusa_need($c[0], $rest * $c[1], $c[2], $avail, $wos, $short, $depth + 1);
    $wos[] = array($sku, $rest, $loc);
}

/** What one order needs: [work orders, shortages]. $avail is updated only when nothing is short. */
function medusa_dispatch_plan($so, &$avail) {
    $try = $avail;
    $wos = array();
    $short = array();
    $res = db_query("SELECT stk_code, quantity - qty_sent q FROM ".TB_PREF."sales_order_details WHERE order_no = "
        .(int)$so['order_no']." AND trans_type = ".ST_SALESORDER." AND quantity > qty_sent ORDER BY id", "order lines");
    while ($r = db_fetch_assoc($res)) medusa_need($r['stk_code'], (float)$r['q'], $so['from_stk_loc'], $try, $wos, $short);
    if (!count($short)) $avail = $try;
    return array($wos, $short);
}

function medusa_qty_text($list) {
    $t = array();
    foreach ($list as $k => $v) $t[] = "$k ".round($v, 4);
    return implode(', ', $t);
}

function medusa_date_open($date) {
    return !is_date_closed($date) && is_date_in_fiscalyears($date, false);
}

/**
 * Posts one order in one transaction: work orders, delivery, invoice,
 * allocation. Returns [ok, text, warning or ''].
 */
function medusa_dispatch_order($so, $wos) {
    global $Refs;
    $ref = $so['customer_ref'];
    $today = Today();
    $inv_date = sql2date($so['ord_date']);
    if (!medusa_date_open($today) || !medusa_date_open($inv_date))
        return array(false, "$ref (SO {$so['order_no']}): $today or $inv_date is not in an open fiscal year; held", '');
    $who = array('customer' => $so['debtor_no'], 'branch' => $so['branch_code']);

    begin_transaction();
    $made = array();
    foreach ($wos as $w) {
        add_work_order($Refs->get_next(ST_WORKORDER, null, $today), $w[2], $w[1], $w[0], WO_ASSEMBLY,
            $today, $today, "Auto-dispatch $ref", 0, '', 0, '');
        $made[] = $w[0].' x'.round($w[1], 4);
    }

    $dn = new Cart(ST_SALESORDER, $so['order_no'], true);
    $dn->document_date = $dn->due_date = $today;
    $dn->reference = $Refs->get_next(ST_CUSTDELIVERY, null, $who + array('date' => $today));
    $delivery = $dn->write(0);

    $iv = new Cart(ST_CUSTDELIVERY, array($delivery), true);
    $iv->document_date = $inv_date;
    $iv->due_date = get_invoice_duedate($iv->payment, $inv_date);
    $iv->reference = $Refs->get_next(ST_SALESINVOICE, null, $who + array('date' => $inv_date));
    $invoice = $iv->write();

    // The order's payments: the import wrote them with memo "<cust_ref> ...".
    $inv = get_customer_trans($invoice, ST_SALESINVOICE);
    $left = round((float)$inv['Total'], 2);
    $paid = 0;
    $res = db_query("SELECT t.trans_no, t.ov_amount + t.ov_discount - t.alloc free FROM ".TB_PREF."debtor_trans t"
        ." JOIN ".TB_PREF."comments c ON c.type = t.type AND c.id = t.trans_no"
        ." WHERE t.type = ".ST_CUSTPAYMENT." AND t.debtor_no = ".db_escape($so['debtor_no'])
        ." AND c.memo_ LIKE ".db_escape($ref.' %')." ORDER BY t.tran_date, t.trans_no", "order payments");
    while ($p = db_fetch_assoc($res)) {
        $free = round((float)$p['free'], 2);
        $paid += $free;
        $amt = min($free, $left);
        if ($amt < 0.005) continue;
        add_cust_allocation($amt, ST_CUSTPAYMENT, $p['trans_no'], ST_SALESINVOICE, $invoice, $so['debtor_no'], $today);
        update_debtor_trans_allocation(ST_CUSTPAYMENT, $p['trans_no'], $so['debtor_no']);
        $left = round($left - $amt, 2);
    }
    update_debtor_trans_allocation(ST_SALESINVOICE, $invoice, $so['debtor_no']);
    commit_transaction();

    $total = round((float)$inv['Total'], 2);
    $warn = abs($paid - $total) < 0.005 ? '' : "$ref (SO {$so['order_no']}, invoice $invoice): invoice \$"
        .number_format($total, 2).", payment(s) \$".number_format($paid, 2)."; the rest is left unallocated";
    return array(true, "$ref (SO {$so['order_no']}) dispatched: delivery $delivery, invoice $invoice dated $inv_date \$"
        .number_format($total, 2).(count($made) ? '; made '.implode(', ', $made) : ''), $warn);
}

/** Dispatches every waiting order; returns [lines, errors]. Off (nothing) when "dispatch from" is blank. */
function medusa_run_dispatch() {
    $lines = array();
    $errors = array();
    $from = medusa_dispatch_from();
    if (!$from) return array($lines, $errors);
    $avail = array();
    foreach (medusa_orders_to_dispatch($from) as $so) {
        list($wos, $short) = medusa_dispatch_plan($so, $avail);
        if (count($short)) {
            $errors[] = "{$so['customer_ref']} (SO {$so['order_no']}) held, short: ".medusa_qty_text($short);
            continue;
        }
        try {
            list($ok, $text, $warn) = medusa_dispatch_order($so, $wos);
        } catch (Throwable $e) {
            cancel_transaction();
            list($ok, $text, $warn) = array(false, "{$so['customer_ref']} (SO {$so['order_no']}) not dispatched: ".$e->getMessage(), '');
        }
        if ($ok) $lines[] = $text; else $errors[] = $text;
        if ($warn != '') $errors[] = $warn;
        $avail = array();   // posted: read stock again
    }
    return array($lines, $errors);
}

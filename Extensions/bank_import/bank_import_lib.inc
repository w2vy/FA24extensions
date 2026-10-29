<?php
/**********************************************
Author: Tom Moulton
Name: Bank statement import (Suncoast CSV, PayPal "Balance affecting" CSV)
Free software under GNU GPLv3

Each statement line is one of: already loaded, already in FA (one FA bank
transaction, or a payout = a set of customer payments), booked in another
bank (PayPal money entered straight into the checking account), created by a
rule, or unknown (tom picks). Lines that were loaded are kept with the FA bank
transactions they stand for, so a statement period can be reconciled when FA
and the bank's running balance agree.
***********************************************/

// ---- tables -----------------------------------------------------------------

function bi_table_exists() {
    $sql = "SHOW TABLES LIKE '".TB_PREF."bank_import_rules'";
    return db_num_rows(db_query($sql, "could not look for the bank_import tables")) > 0;
}

function bi_create_tables() {
    db_query("CREATE TABLE IF NOT EXISTS ".TB_PREF."bank_import_lines ("
        ." `id` int NOT NULL AUTO_INCREMENT, `bank_act` int NOT NULL, `line_key` varchar(64) NOT NULL,"
        ." `posted` date NOT NULL, `descr` varchar(255) NOT NULL default '', `amount` double NOT NULL,"
        ." `balance` double NOT NULL, `status` varchar(12) NOT NULL, `reconciled` date NULL,"
        ." `upload_id` int NOT NULL default 0,"
        ." PRIMARY KEY (`id`), UNIQUE KEY `bank_key` (`bank_act`, `line_key`), KEY `posted` (`bank_act`, `posted`))",
        "Error creating bank_import_lines");
    db_query("CREATE TABLE IF NOT EXISTS ".TB_PREF."bank_import_links ("
        ." `line_id` int NOT NULL, `bank_trans_id` int NOT NULL,"
        ." PRIMARY KEY (`line_id`, `bank_trans_id`), KEY `bank_trans` (`bank_trans_id`))",
        "Error creating bank_import_links");
    db_query("CREATE TABLE IF NOT EXISTS ".TB_PREF."bank_import_rules ("
        ." `id` int NOT NULL AUTO_INCREMENT, `priority` int NOT NULL default 50, `bank_act` int NOT NULL default 0,"
        ." `pattern` varchar(100) NOT NULL, `sign` tinyint NOT NULL default 0, `action` varchar(12) NOT NULL,"
        ." `target` varchar(20) NOT NULL default '', `inactive` tinyint NOT NULL default 0,"
        ." PRIMARY KEY (`id`))",
        "Error creating bank_import_rules");
}

// ---- parsing ----------------------------------------------------------------

/** "$1,009.12" / "($3.99)" / "-61.20" -> float. */
function bi_money($s) {
    $s = trim($s);
    if ($s == '') return 0.0;
    $neg = strpos($s, '(') !== false || strpos($s, '-') !== false;
    $v = (float)preg_replace('/[^0-9.]/', '', $s);
    return $neg ? -$v : $v;
}

/** m/d/Y -> Y-m-d, or '' when it isn't one. */
function bi_mdy($s) {
    if (!preg_match('#^(\d{1,2})/(\d{1,2})/(\d{4})$#', trim($s), $m)) return '';
    return sprintf('%04d-%02d-%02d', $m[3], $m[1], $m[2]);
}

/**
 * The statement lines of a Suncoast or PayPal CSV, oldest first:
 * [key, posted (Y-m-d), tdate, descr, amount (the bank's change), balance,
 *  and for PayPal: gross, fee, email, ptype, txid, reftx]. Returns [lines, error].
 * The running balance must chain (each Balance = the one before + amount):
 * a break means a missing or reordered line, and nothing is loaded.
 */
function bi_parse($path) {
    $fp = @fopen($path, 'r');
    if (!$fp) return array(array(), "could not open the file");
    $head = fgetcsv($fp, 0, ',', '"', '');
    if (!$head) return array(array(), "empty file");
    $head[0] = preg_replace('/^\xEF\xBB\xBF/', '', $head[0]);  // UTF-8 BOM
    $col = array_flip(array_map('trim', $head));
    $rows = array();
    while (($r = fgetcsv($fp, 0, ',', '"', '')) !== false) {
        if (count($r) < 2 || trim(implode('', $r)) == '') continue;
        $rows[] = $r;
    }
    fclose($fp);
    $f = function ($r, $name) use ($col) { return isset($col[$name], $r[$col[$name]]) ? trim($r[$col[$name]]) : ''; };

    $lines = array();
    if (isset($col['Posted Date'], $col['Description'], $col['Balance'])) {
        // Suncoast: newest first.
        foreach (array_reverse($rows) as $r) {
            $posted = bi_mdy($f($r, 'Posted Date'));
            if ($posted == '') return array(array(), "bad Posted Date: ".implode(',', $r));
            $amount = round(abs(bi_money($f($r, 'Deposit'))) - abs(bi_money($f($r, 'Withdrawal'))), 2);
            $balance = round(bi_money($f($r, 'Balance')), 2);
            $descr = preg_replace('/\s+/', ' ', $f($r, 'Description'));
            $lines[] = array('key' => sha1("$posted|$descr|".number_format($amount, 2, '.', '')."|".number_format($balance, 2, '.', '')),
                'posted' => $posted, 'tdate' => bi_mdy($f($r, 'Transaction Date')), 'descr' => $descr,
                'amount' => $amount, 'balance' => $balance);
        }
    } elseif (isset($col['Transaction ID'], $col['Net'], $col['Balance'])) {
        // PayPal "Balance affecting": completed rows only; Memo rows don't move the balance.
        foreach ($rows as $r) {
            if ($f($r, 'Status') != 'Completed' || $f($r, 'Balance Impact') == 'Memo') continue;
            $posted = bi_mdy($f($r, 'Date'));
            if ($posted == '') return array(array(), "bad Date: ".implode(',', $r));
            $net = round(bi_money($f($r, 'Net')), 2);
            $memo = array();
            foreach (array('Item Title', 'Subject', 'Note') as $k) if ($f($r, $k) != '') $memo[] = $f($r, $k);
            $descr = $f($r, 'Type') . ($f($r, 'Name') != '' ? ' | '.$f($r, 'Name') : '') . (count($memo) ? ' | '.implode(' | ', $memo) : '');
            $lines[] = array('key' => 'PP:'.$f($r, 'Transaction ID'), 'posted' => $posted, 'tdate' => $posted,
                'descr' => substr($descr, 0, 255), 'amount' => $net, 'balance' => round(bi_money($f($r, 'Balance')), 2),
                'gross' => round(bi_money($f($r, 'Gross')), 2), 'fee' => round(bi_money($f($r, 'Fee')), 2),
                'email' => $net > 0 ? $f($r, 'From Email Address') : $f($r, 'To Email Address'),
                'ptype' => $f($r, 'Type'), 'txid' => $f($r, 'Transaction ID'), 'reftx' => $f($r, 'Reference Txn ID'));
        }
        if (count($lines) > 1 && $lines[0]['posted'] > $lines[count($lines) - 1]['posted'])
            $lines = array_reverse($lines);
    } else
        return array(array(), "not a Suncoast or PayPal CSV (header: ".implode(',', $head).")");

    for ($i = 1; $i < count($lines); $i++) {
        $want = round($lines[$i - 1]['balance'] + $lines[$i]['amount'], 2);
        if (abs($want - $lines[$i]['balance']) > 0.004)
            return array(array(), "running balance breaks at ".$lines[$i]['posted']." ".$lines[$i]['descr']
                .": ".number_format($lines[$i - 1]['balance'], 2)." + ".number_format($lines[$i]['amount'], 2)
                ." = ".number_format($want, 2).", file says ".number_format($lines[$i]['balance'], 2)
                .". A line is missing or out of order; nothing loaded.");
    }
    return array($lines, '');
}

// ---- matching ---------------------------------------------------------------

/** Unreconciled FA bank transactions not yet tied to a statement line (and not taken in this run). */
function bi_free_bank_trans_sql($bank, $taken) {
    $sql = "SELECT b.id, b.type, b.trans_no, b.trans_date, b.amount, b.person_type_id, b.person_id FROM ".TB_PREF."bank_trans b"
        ." LEFT JOIN ".TB_PREF."voided v ON v.type = b.type AND v.id = b.trans_no"
        ." LEFT JOIN ".TB_PREF."bank_import_links l ON l.bank_trans_id = b.id"
        ." WHERE b.bank_act = ".db_escape($bank)." AND v.id IS NULL AND l.line_id IS NULL"
        ." AND b.reconciled IS NULL AND b.amount != 0";
    if (count($taken)) $sql .= " AND b.id NOT IN (".implode(',', array_map('intval', array_keys($taken))).")";
    return $sql;
}

/** The FA bank transaction for this line: same amount, FA date from 7 days before to 3 after; closest date wins. */
function bi_match_one($bank, $line, $taken) {
    $p = db_escape($line['posted']);
    $sql = bi_free_bank_trans_sql($bank, $taken)
        ." AND ROUND(b.amount, 2) = ".round($line['amount'], 2)
        ." AND b.trans_date BETWEEN DATE_SUB($p, INTERVAL 7 DAY) AND DATE_ADD($p, INTERVAL 3 DAY)"
        ." ORDER BY ABS(DATEDIFF(b.trans_date, $p)), b.id";
    $res = db_query($sql, "could not look for a matching bank transaction");
    $all = array();
    while ($r = db_fetch_assoc($res)) $all[] = $r;
    return $all;
}

/**
 * A payout: unreconciled customer payments of the last 10 days that add up to it,
 * less any processing fees kept in that time (a bank payment whose only other GL
 * line is the bank charges account, e.g. a refunded duplicate charge's fee).
 */
function bi_match_payout($bank, $line, $taken) {
    $p = db_escape($line['posted']);
    $fee_act = db_escape(get_company_pref('bank_charge_act'));
    $bank_gl = db_escape(get_bank_gl_account($bank));
    $sql = bi_free_bank_trans_sql($bank, $taken)
        ." AND ((b.type = ".ST_CUSTPAYMENT." AND b.amount > 0) OR (b.type = ".ST_BANKPAYMENT." AND b.amount < 0"
        ." AND NOT EXISTS (SELECT 1 FROM ".TB_PREF."gl_trans g WHERE g.type = b.type AND g.type_no = b.trans_no"
        ." AND g.amount != 0 AND g.account NOT IN ($fee_act, $bank_gl))))"
        ." AND b.trans_date BETWEEN DATE_SUB($p, INTERVAL 10 DAY) AND $p ORDER BY b.trans_date DESC, b.id DESC LIMIT 20";
    $res = db_query($sql, "could not look for payout payments");
    $cand = array();
    while ($r = db_fetch_assoc($res)) $cand[] = $r;
    $want = (int)round($line['amount'] * 100);
    $pick = array();
    // Fees are negative, so a partial sum may pass the target and come back: no pruning on the sign.
    $find = function ($i, $left) use (&$find, &$pick, $cand) {
        if ($left == 0 && count($pick) > 0) return true;
        if ($i >= count($cand)) return false;
        $c = (int)round($cand[$i]['amount'] * 100);
        $pick[] = $cand[$i];
        if ($find($i + 1, $left - $c)) return true;
        array_pop($pick);
        return $find($i + 1, $left);
    };
    return $find(0, $want) ? $pick : array();
}

/** PayPal money booked as a customer payment in ANOTHER FA bank (Woo #1709 -> bank 8). */
function bi_booked_elsewhere($bank, $line) {
    $p = db_escape($line['posted']);
    $sql = "SELECT b.bank_act, b.type, b.trans_no FROM ".TB_PREF."bank_trans b"
        ." LEFT JOIN ".TB_PREF."voided v ON v.type = b.type AND v.id = b.trans_no"
        ." WHERE b.bank_act != ".db_escape($bank)." AND v.id IS NULL AND b.type = ".ST_CUSTPAYMENT
        ." AND ROUND(b.amount, 2) = ".round($line['amount'], 2)
        ." AND b.trans_date BETWEEN DATE_SUB($p, INTERVAL 7 DAY) AND DATE_ADD($p, INTERVAL 7 DAY) LIMIT 1";
    return db_fetch_assoc(db_query($sql, "could not look in the other banks"));
}

function bi_rules($bank) {
    $res = db_query("SELECT * FROM ".TB_PREF."bank_import_rules WHERE inactive = 0 AND bank_act IN (0, "
        .db_escape($bank).") ORDER BY priority, id", "could not read the rules");
    $rules = array();
    while ($r = db_fetch_assoc($res)) $rules[] = $r;
    return $rules;
}

/** The FA customer for this email (via CRM persons, as medusa_customer), or 0. */
function bi_customer_by_email($email) {
    if ($email == '') return 0;
    $res = db_query("SELECT C.type, C.entity_id FROM ".TB_PREF."crm_persons AS P LEFT JOIN ".TB_PREF."crm_contacts AS C"
        ." ON P.id = C.person_id WHERE P.email = ".db_escape($email), "customer could not be retrieved");
    while ($c = db_fetch_assoc($res)) {
        if ($c['type'] == 'customer') return (int)$c['entity_id'];
        if ($c['type'] == 'cust_branch') {
            $b = db_fetch_assoc(db_query("SELECT debtor_no FROM ".TB_PREF."cust_branch WHERE branch_code = "
                .db_escape($c['entity_id']), "branch"));
            if ($b) return (int)$b['debtor_no'];
        }
    }
    return 0;
}

/** A rule pattern from a description: card prefixes, numbers and refs dropped (tom edits it). */
function bi_pattern_guess($descr) {
    $skip = array('withdrawal', 'deposit', 'recurring', 'debit', 'card', 'pos', 'ach', 'home', 'banking', 'bill',
        'payment', 'adjustment', 'credit', 'voucher');
    $out = array();
    foreach (preg_split('/[\s|]+/', $descr) as $t) {
        if (!count($out) && (in_array(strtolower($t), $skip) || $t == '' || $t[0] == '#')) continue;
        if (preg_match('/\d/', $t, $m, PREG_OFFSET_CAPTURE)) {
            $t = substr($t, 0, $m[0][1]);
            if (($s = strpos($t, '*')) !== false && strlen($t) - $s - 1 < 3) $t = substr($t, 0, $s + 1);
            if (strlen($t) >= 3) $out[] = $t;
            break;
        }
        $out[] = $t;
        if (count($out) == 2) break;
    }
    return implode(' ', $out);
}

/**
 * What each line will do. Adds 'plan' => [status, note, bank_trans rows, action, target]
 * where status = loaded | matched | group | elsewhere | rule | unknown.
 */
function bi_plan($bank, $lines) {
    $rules = bi_rules($bank);
    $taken = array();
    $elsewhere_out = array();   // PayPal: amounts booked elsewhere whose withdrawal is still to come
    foreach ($lines as $i => $l) {
        $plan = array('status' => 'unknown', 'note' => '', 'bt' => array(), 'action' => 'leave', 'target' => '');
        $known = db_fetch_row(db_query("SELECT status FROM ".TB_PREF."bank_import_lines WHERE bank_act = "
            .db_escape($bank)." AND line_key = ".db_escape($l['key']), "could not look up the line"));
        if ($known) {
            $plan['status'] = 'loaded';
            $plan['note'] = "already loaded (".$known[0].")";
            $lines[$i]['plan'] = $plan;
            continue;
        }
        $one = bi_match_one($bank, $l, $taken);
        if (count($one)) {
            $plan['status'] = 'matched';
            $plan['bt'] = array($one[0]);
            $taken[$one[0]['id']] = 1;
            if (count($one) > 1) $plan['note'] = (count($one) - 1)." other(s) with the same amount; closest date used";
            $lines[$i]['plan'] = $plan;
            continue;
        }
        if (isset($l['txid'])) {
            $cents = (string)round(abs($l['amount']) * 100);
            if ($l['amount'] > 0 && stripos($l['ptype'], 'Payment') !== false && ($e = bi_booked_elsewhere($bank, $l))) {
                $plan['status'] = 'elsewhere';
                $plan['note'] = "booked in bank ".$e['bank_act']." (".$e['type']."/".$e['trans_no'].")";
                $elsewhere_out[$cents] = $l['posted'];
                $lines[$i]['plan'] = $plan;
                continue;
            }
            if ($l['amount'] < 0 && isset($elsewhere_out[$cents])) {
                $plan['status'] = 'elsewhere';
                $plan['note'] = "withdrawal of the payment booked in another bank";
                unset($elsewhere_out[$cents]);
                $lines[$i]['plan'] = $plan;
                continue;
            }
        }
        $sign = $l['amount'] > 0 ? 1 : -1;
        foreach ($rules as $r) {
            if ($r['sign'] != 0 && $r['sign'] != $sign) continue;
            if (stripos($l['descr'], $r['pattern']) === false) continue;
            if ($r['action'] == 'payout') {
                $set = bi_match_payout($bank, $l, $taken);
                if (count($set)) {
                    $plan['status'] = 'group';
                    $plan['bt'] = $set;
                    foreach ($set as $b) $taken[$b['id']] = 1;
                    $plan['note'] = count($set)." customer payment(s) add up to it";
                } else
                    $plan['note'] = "payout rule \"".$r['pattern']."\": no set of customer payments adds up to it";
                break;
            }
            $plan['status'] = 'rule';
            $plan['action'] = $r['action'];
            $plan['target'] = $r['target'];
            $plan['note'] = "rule \"".$r['pattern']."\"";
            break;
        }
        if (($plan['status'] == 'rule' || $plan['status'] == 'unknown') && isset($l['email']) && $l['amount'] > 0) {
            $cust = bi_customer_by_email($l['email']);
            if ($cust) {
                if ($plan['action'] == 'customer' && $plan['target'] == '') $plan['target'] = $cust;
                $plan['note'] .= ($plan['note'] != '' ? '; ' : '')."email = customer $cust";
            }
        }
        $lines[$i]['plan'] = $plan;
    }
    return $lines;
}

// ---- posting ----------------------------------------------------------------

/** The bank_trans id of an FA transaction in this bank. */
function bi_bank_trans_id($bank, $type, $trans_no) {
    $r = db_fetch_row(db_query("SELECT id FROM ".TB_PREF."bank_trans WHERE bank_act = ".db_escape($bank)
        ." AND type = ".db_escape($type)." AND trans_no = ".db_escape($trans_no), "could not find the new bank transaction"));
    return $r ? (int)$r[0] : 0;
}

/** Why this action can't post this line, or ''. */
function bi_check($bank, $l, $action, $target) {
    $date = sql2date($l['posted']);
    if (!is_date_in_fiscalyears($date, false)) return "date not in an open fiscal year";
    if (is_date_closed($date)) return "date is in a closed period";
    $amt = $l['amount'];
    switch ($action) {
        case 'skip': return '';
        case 'qe':
            $qe = get_quick_entry($target);
            if (!$qe) return "no quick entry $target";
            if ($qe['type'] != QE_PAYMENT && $qe['type'] != QE_DEPOSIT) return "quick entry $target is not a payment/deposit one";
            return '';
        case 'gl':
            return get_gl_account($target) ? '' : "no GL account $target";
        case 'supplier':
            if ($amt >= 0) return "a supplier payment must be money out";
            return get_supplier($target) ? '' : "no supplier $target";
        case 'customer':
            if ($amt <= 0) return "a customer payment must be money in";
            if (!get_customer($target)) return "no customer $target";
            return db_num_rows(db_query("SELECT 1 FROM ".TB_PREF."cust_branch WHERE debtor_no = ".db_escape($target),
                "branch")) ? '' : "customer $target has no branch";
        case 'transfer':
            if ($target == $bank) return "transfer to the same bank";
            return get_bank_account($target) ? '' : "no bank account $target";
    }
    return "unknown action $action";
}

/** Post one line; returns the new bank_trans id in this bank (0 for skip). */
function bi_post($bank, $l, $action, $target) {
    global $Refs;
    $date = sql2date($l['posted']);
    $memo = 'BANK: '.$l['descr'];
    if (!empty($l['tdate']) && $l['tdate'] != $l['posted']) $memo .= ' (on '.sql2date($l['tdate']).')';
    $amt = $l['amount'];
    // PayPal rows: the payee gets the gross, PayPal's fee goes to bank charges; the bank moves by the net.
    $gross = isset($l['gross']) ? $l['gross'] : $amt;
    $fee = isset($l['fee']) ? $l['fee'] : 0;

    if ($action == 'skip') return 0;
    if ($action == 'qe' || $action == 'gl') {
        $type = $amt < 0 ? ST_BANKPAYMENT : ST_BANKDEPOSIT;
        $cart = new items_cart($type);
        $cart->tran_date = $date;
        // A payment's GL lines are positive, a deposit's negative (the bank gets minus their total).
        $s = $amt < 0 ? 1 : -1;
        if ($action == 'qe') {
            display_quick_entries($cart, $target, abs($gross), QE_PAYMENT, $memo);
            foreach ($cart->gl_items as $k => $it) $cart->gl_items[$k]->amount = $s * $it->amount;
            $ptype = PT_QUICKENTRY;
            $pid = $target;
        } else {
            $cart->add_gl_item($target, 0, 0, $s * abs($gross), $memo);
            $ptype = PT_MISC;
            $pid = substr($l['descr'], 0, 40);
        }
        if ($fee != 0) $cart->add_gl_item(get_company_pref('bank_charge_act'), 0, 0, -$fee, 'PayPal fee');
        $ref = $Refs->get_next($type, null, $date);
        list($t, $no) = write_bank_transaction($type, 0, $bank, $cart, $date, $ptype, $pid, 0, $ref, $memo, true, null);
        return bi_bank_trans_id($bank, $t, $no);
    }
    if ($action == 'supplier') {
        $ref = $Refs->get_next(ST_SUPPAYMENT, null, array('supplier' => $target, 'date' => $date));
        $no = write_supp_payment(0, $target, $bank, $date, $ref, abs($amt), 0, $memo);
        return bi_bank_trans_id($bank, ST_SUPPAYMENT, $no);
    }
    if ($action == 'customer') {
        $b = db_fetch_assoc(db_query("SELECT branch_code FROM ".TB_PREF."cust_branch WHERE debtor_no = "
            .db_escape($target)." ORDER BY branch_code LIMIT 1", "branch"));
        $ref = $Refs->get_next(ST_CUSTPAYMENT, null, array('customer' => $target, 'branch' => $b['branch_code'], 'date' => $date));
        $no = write_customer_payment(0, $target, $b['branch_code'], $bank, $date, $ref, abs($gross), 0, $memo, 0, -$fee);
        return bi_bank_trans_id($bank, ST_CUSTPAYMENT, $no);
    }
    if ($action == 'transfer') {
        list($from, $to) = $amt < 0 ? array($bank, $target) : array($target, $bank);
        $ref = $Refs->get_next(ST_BANKTRANSFER, null, $date);
        // All 10 args: FA array_combine()s func_get_args() with 10 names.
        $no = add_bank_transfer($from, $to, $date, abs($amt), $ref, $memo, 0, 0, 0, 0);
        return bi_bank_trans_id($bank, ST_BANKTRANSFER, $no);
    }
    return 0;
}

function bi_save_line($bank, $l, $status, $bt_ids, $upload_id) {
    db_query("INSERT INTO ".TB_PREF."bank_import_lines (bank_act, line_key, posted, descr, amount, balance, status, upload_id)"
        ." VALUES (".db_escape($bank).", ".db_escape($l['key']).", ".db_escape($l['posted']).", ".db_escape($l['descr'])
        .", ".round($l['amount'], 2).", ".round($l['balance'], 2).", ".db_escape($status).", ".(int)$upload_id.")",
        "could not save the statement line");
    $id = db_insert_id();
    foreach ($bt_ids as $b) if ($b)
        db_query("INSERT INTO ".TB_PREF."bank_import_links (line_id, bank_trans_id) VALUES ($id, ".(int)$b.")",
            "could not link the statement line");
    return $id;
}

// ---- reconcile --------------------------------------------------------------

/**
 * The reconcile picture for a bank up to a date (Y-m-d): [lines, FA reconciled balance,
 * statement opening, statement end, FA balance after, problems].
 */
function bi_reconcile_plan($bank, $to) {
    $fa_rec = (float)db_fetch_row(db_query("SELECT IFNULL(SUM(amount), 0) FROM ".TB_PREF."bank_trans WHERE bank_act = "
        .db_escape($bank)." AND reconciled IS NOT NULL", "reconciled balance"))[0];
    $res = db_query("SELECT * FROM ".TB_PREF."bank_import_lines WHERE bank_act = ".db_escape($bank)
        ." AND reconciled IS NULL AND posted <= ".db_escape($to)." ORDER BY posted, id", "statement lines");
    $lines = array();
    while ($r = db_fetch_assoc($res)) $lines[] = $r;
    $problems = array();
    if (!count($lines)) return array($lines, $fa_rec, 0, 0, $fa_rec, array("no loaded lines to reconcile up to ".sql2date($to)));
    $open = round($lines[0]['balance'] - $lines[0]['amount'], 2);
    $end = round($lines[count($lines) - 1]['balance'], 2);
    $fa_after = $fa_rec;
    $sum = 0;
    foreach ($lines as $l) $sum += $l['amount'];
    $res = db_query("SELECT IFNULL(SUM(b.amount), 0) FROM ".TB_PREF."bank_trans b JOIN ".TB_PREF."bank_import_links k"
        ." ON k.bank_trans_id = b.id JOIN ".TB_PREF."bank_import_lines l ON l.id = k.line_id"
        ." WHERE l.bank_act = ".db_escape($bank)." AND l.reconciled IS NULL AND l.posted <= ".db_escape($to)
        ." AND b.reconciled IS NULL", "linked total");
    $fa_after += (float)db_fetch_row($res)[0];
    if (abs($open - $fa_rec) > 0.004)
        $problems[] = "the statement opens at ".number_format($open, 2)." but FA's reconciled balance is "
            .number_format($fa_rec, 2).": load the statement from the day after FA's last reconcile";
    if (abs(round($open + $sum, 2) - $end) > 0.004)
        $problems[] = "lines are missing between ".sql2date($lines[0]['posted'])." and ".sql2date($to)
            ." (opening + lines = ".number_format($open + $sum, 2).", statement says ".number_format($end, 2)
            ."): load the unknown lines first";
    if (abs(round($fa_after, 2) - $end) > 0.004)
        $problems[] = "FA would end at ".number_format($fa_after, 2).", the bank at ".number_format($end, 2)
            ." (difference ".number_format($end - $fa_after, 2).")";
    return array($lines, $fa_rec, $open, $end, round($fa_after, 2), $problems);
}

/** FA bank transactions up to the date that no statement line stands for. */
function bi_fa_not_at_bank($bank, $to) {
    $sql = bi_free_bank_trans_sql($bank, array())." AND b.trans_date <= ".db_escape($to)." ORDER BY b.trans_date, b.id";
    $res = db_query($sql, "FA lines not at the bank");
    $out = array();
    while ($r = db_fetch_assoc($res)) $out[] = $r;
    return $out;
}

function bi_reconcile($bank, $to, $lines, $end) {
    $date = sql2date($to);
    begin_transaction();
    foreach ($lines as $l) {
        $res = db_query("SELECT k.bank_trans_id FROM ".TB_PREF."bank_import_links k JOIN ".TB_PREF."bank_trans b"
            ." ON b.id = k.bank_trans_id WHERE k.line_id = ".(int)$l['id']." AND b.reconciled IS NULL", "links");
        while ($r = db_fetch_row($res))
            update_reconciled_values($r[0], db_escape($to), $date, $end, $bank);
        db_query("UPDATE ".TB_PREF."bank_import_lines SET reconciled = ".db_escape($to)." WHERE id = ".(int)$l['id'],
            "could not mark the line reconciled");
    }
    // Also stamps the bank's last reconcile even when every line was elsewhere/skip.
    db_query("UPDATE ".TB_PREF."bank_accounts SET last_reconciled_date = ".db_escape($to).", ending_reconcile_balance = "
        .round($end, 2)." WHERE id = ".db_escape($bank), "could not save the reconcile date");
    commit_transaction();
}

// ---- starter rules (tom 10-29) ---------------------------------------------

function bi_starter_rules() {
    // [priority, bank, pattern, sign, action, target]
    return array(
        array(10, 8, 'NSF FEE', -1, 'qe', '11'),
        array(20, 8, 'Vast.ai', 1, 'qe', '25'),
        array(20, 8, 'CLOUD.RUNONFLUX.IO', 0, 'qe', '15'),
        array(20, 8, 'WP*WOOCOMMER', 0, 'qe', '15'),
        array(20, 8, 'ANTHROPIC', 0, 'qe', '15'),
        array(20, 8, 'FRONTIER COMMUNI', -1, 'qe', '15'),
        array(20, 8, 'Duke Energy', -1, 'qe', '21'),
        array(20, 8, 'FEDEX', -1, 'qe', '5'),
        array(20, 8, 'SALEFABR', -1, 'supplier', '1'),
        array(30, 8, 'WooPayments', 1, 'payout', ''),
        array(30, 8, 'STRIPE', 1, 'payout', ''),
        array(30, 8, 'PAYPAL', 1, 'transfer', '3'),
        array(10, 3, 'User Initiated Withdrawal', -1, 'transfer', '8'),
        array(10, 3, 'General Card Deposit', 1, 'transfer', '8'),
        array(20, 3, 'General Payment', 1, 'customer', ''),
        array(20, 3, 'U.S. Postal Service', -1, 'qe', '5'),
    );
}

function bi_add_rule($priority, $bank, $pattern, $sign, $action, $target) {
    db_query("INSERT INTO ".TB_PREF."bank_import_rules (priority, bank_act, pattern, sign, action, target) VALUES ("
        .(int)$priority.", ".(int)$bank.", ".db_escape($pattern).", ".(int)$sign.", ".db_escape($action).", "
        .db_escape($target).")", "could not add the rule");
}

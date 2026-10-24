<?php
/**********************************************
Author: Tom Moulton
Name: Medusa shop interface for FrontAccounting (GotGrit)
Free software under GNU GPLv3

FA connects OUT to the Medusa Admin API (secret API key, HTTP Basic); the shop
never calls FA. Orders that are paid AND fulfilled/shipped become FA sales
orders (cust_ref "Medusa Order # N"), each Stripe payment an unallocated FA
customer payment with the Stripe fee as bank charge.
***********************************************/

$MEDUSA_REF = 'Medusa Order # ';

// ---- settings (table <pref>medusa: name/value) ------------------------------

function medusa_table_exists() {
    $sql = "SHOW TABLES LIKE '".TB_PREF."medusa'";
    return db_num_rows(db_query($sql, "could not look for the medusa table")) > 0;
}

function medusa_create_table() {
    db_query("CREATE TABLE IF NOT EXISTS ".TB_PREF."medusa ( `name` char(20) NOT NULL default '', " .
        " `value` varchar(255) NOT NULL default '', PRIMARY KEY (`name`))", "Error creating table");
}

function medusa_get($name, $default = '') {
    $res = db_query("SELECT value FROM ".TB_PREF."medusa WHERE name = ".db_escape($name), "could not read setting");
    $row = db_fetch_row($res);
    return is_array($row) ? $row[0] : $default;
}

function medusa_set($name, $value) {
    db_query("REPLACE INTO ".TB_PREF."medusa (name, value) VALUES (".db_escape($name).", ".db_escape($value).")",
        "could not write setting");
}

// ---- HTTP -------------------------------------------------------------------

/** GET/POST JSON; returns [http status, decoded body or null, error text]. */
function medusa_http($method, $url, $userpwd, $body = null) {
    $c = curl_init($url);
    curl_setopt($c, CURLOPT_RETURNTRANSFER, 1);
    curl_setopt($c, CURLOPT_USERPWD, $userpwd);
    curl_setopt($c, CURLOPT_TIMEOUT, 60);
    curl_setopt($c, CURLOPT_CUSTOMREQUEST, $method);
    if ($body !== null) {
        curl_setopt($c, CURLOPT_POSTFIELDS, json_encode($body));
        curl_setopt($c, CURLOPT_HTTPHEADER, array('Content-Type: application/json'));
    }
    $raw = curl_exec($c);
    $status = curl_getinfo($c, CURLINFO_HTTP_CODE);
    $err = $raw === false ? curl_error($c) : '';
    curl_close($c);
    $json = $raw === false ? null : json_decode($raw, true);
    if ($status >= 400 && $err == '') $err = is_array($json) && isset($json['message']) ? $json['message'] : "HTTP $status";
    return array($status, $json, $err);
}

function medusa_api($method, $path, $query = array(), $body = null) {
    $url = rtrim(medusa_get('url'), '/') . $path;
    if (count($query)) $url .= '?' . http_build_query($query);
    return medusa_http($method, $url, medusa_get('apikey') . ':', $body);
}

/** The Stripe fee of a charge, or null when unknown (no key, no charge, API error). */
function medusa_stripe_fee($charge_id) {
    $key = medusa_get('stripekey');
    if ($key == '' || $charge_id == '') return null;
    list($status, $ch, $err) = medusa_http('GET',
        'https://api.stripe.com/v1/charges/' . urlencode($charge_id) . '?expand[]=balance_transaction', $key . ':');
    if ($err != '' || !isset($ch['balance_transaction']['fee'])) return null;
    return $ch['balance_transaction']['fee'] / 100.0;
}

// ---- orders -----------------------------------------------------------------

$MEDUSA_ORDER_FIELDS = 'id,display_id,email,created_at,currency_code,status,payment_status,fulfillment_status,'
    . 'shipping_subtotal,tax_total,total,*shipping_address,*billing_address,'
    . 'items.variant_sku,items.title,items.quantity,items.unit_price,items.subtotal,'
    . 'items.tax_lines.code,items.variant.metadata,'
    . 'payment_collections.payments.id,payment_collections.payments.amount,payment_collections.payments.data,'
    . 'payment_collections.payments.captured_at,payment_collections.payments.canceled_at';

/** Paid (captured, or since refunded) AND fulfilled (label bought) or shipped/delivered. */
function medusa_order_ready($o) {
    return $o['status'] != 'canceled'
        && in_array($o['payment_status'], array('captured', 'partially_refunded', 'refunded'))
        && in_array($o['fulfillment_status'], array('fulfilled', 'shipped', 'delivered'));
}

/** Orders after the cursor that are ready and not yet in FA, oldest first. */
function medusa_orders_to_import($cursor, &$error) {
    global $MEDUSA_ORDER_FIELDS, $MEDUSA_REF;
    $out = array();
    $offset = 0;
    do {
        list($status, $page, $err) = medusa_api('GET', '/admin/orders', array(
            'order' => 'display_id', 'limit' => 50, 'offset' => $offset, 'fields' => $MEDUSA_ORDER_FIELDS));
        if ($err != '') { $error = "Medusa: $err"; return $out; }
        foreach ($page['orders'] as $o) {
            if ((int)$o['display_id'] <= (int)$cursor || !medusa_order_ready($o)) continue;
            $res = db_query("SELECT order_no FROM ".TB_PREF."sales_orders WHERE trans_type = ".ST_SALESORDER
                ." AND customer_ref = ".db_escape($MEDUSA_REF.$o['display_id']), "could not look for the order");
            if (db_num_rows($res) > 0) continue;
            $out[] = $o;
        }
        $offset += 50;
    } while ($offset < $page['count']);
    return $out;
}

function medusa_address_format($a) {
    $ret = '';
    if (!empty($a['company'])) $ret .= $a['company'] . "\n";
    $ret .= trim($a['first_name'] . ' ' . $a['last_name']) . "\n" . $a['address_1'] . "\n";
    if (!empty($a['address_2'])) $ret .= $a['address_2'] . "\n";
    $ret .= $a['city'];
    if (!empty($a['province'])) $ret .= ", " . strtoupper($a['province']);
    $ret .= " " . $a['postal_code'] . "\n";
    // Always the country code, as woo_orders wrote it ("US"), so a returning customer's address matches their branch.
    if (!empty($a['country_code'])) $ret .= strtoupper($a['country_code']) . "\n";
    return $ret;
}

/** The FA tax group named by the order's tax lines (US-FL-<County>), else the default. */
function medusa_tax_group($o, $default) {
    foreach ($o['items'] as $it) {
        foreach ((array)$it['tax_lines'] as $t) {
            $res = db_query("SELECT id FROM ".TB_PREF."tax_groups WHERE name = ".db_escape($t['code']), "tax group");
            $row = db_fetch_row($res);
            if (is_array($row)) return (int)$row[0];
        }
    }
    return (int)$default;
}

/** A unique debtor/branch ref: the name, or name_N when taken (as woo_orders). */
function medusa_unique_ref($table, $col, $name) {
    $res = db_query("SELECT * FROM ".TB_PREF."$table WHERE `$col` LIKE ".db_escape($name."%"), "ref lookup");
    $n = db_num_rows($res);
    return $n > 0 ? $name.'_'.($n+1) : $name;
}

/** The FA customer for this email (via CRM persons), created when new. */
function medusa_customer($email, $name, $addr, $taxgid, $opts) {
    $res = db_query("SELECT C.type, C.entity_id FROM ".TB_PREF."crm_persons AS P LEFT JOIN ".TB_PREF."crm_contacts AS C"
        ." ON P.id = C.person_id WHERE P.email = ".db_escape($email), "customer could not be retrieved");
    while ($c = db_fetch_assoc($res)) {
        if ($c['type'] == 'customer') return array((int)$c['entity_id'], false);
        if ($c['type'] == 'cust_branch') {
            $b = db_fetch_assoc(db_query("SELECT debtor_no FROM ".TB_PREF."cust_branch WHERE branch_code = "
                .db_escape($c['entity_id']), "branch"));
            if ($b) return array((int)$b['debtor_no'], false);
        }
    }
    add_customer($name, medusa_unique_ref('debtors_master', 'debtor_ref', $name), $addr, '', $opts['currency'],
        0, 0, 1, $opts['payment_terms'], 0, 0, $opts['credit_limit'], $opts['sales_type'], NULL);
    $debtor_no = db_insert_id();
    add_branch($debtor_no, $name, medusa_unique_ref('cust_branch', 'branch_ref', $name), $addr, $opts['salesman'],
        $opts['area'], $taxgid, $opts['sales_account'], $opts['sales_discount_account'], $opts['receivables_account'],
        $opts['payment_discount_account'], $opts['default_location'], $addr, 0, 1, '', NULL);
    $branch_code = db_insert_id();
    $person_id = add_crm_person($name, $name, '', $addr, '', '', '', $email, '', '');
    add_crm_contact('cust_branch', 'general', $branch_code, $person_id);
    add_crm_contact('customer', 'general', $debtor_no, $person_id);
    return array($debtor_no, true);
}

/** The customer's branch at this address; a new address = a new branch (copied from the first). */
function medusa_branch($debtor_no, $addr, $taxgid) {
    $res = db_query("SELECT * FROM ".TB_PREF."cust_branch WHERE debtor_no = ".db_escape($debtor_no)
        ." AND br_address = ".db_escape($addr), "could not find customer branch");
    if ($b = db_fetch_assoc($res)) return $b;
    $old = db_fetch_assoc(db_query("SELECT * FROM ".TB_PREF."cust_branch WHERE debtor_no = ".db_escape($debtor_no),
        "could not find any customer branch"));
    if (!$old) return null;
    add_branch($debtor_no, $old['br_name'], $old['branch_ref'], $addr, $old['salesman'], $old['area'], $taxgid,
        $old['sales_account'], $old['sales_discount_account'], $old['receivables_account'],
        $old['payment_discount_account'], $old['default_location'], $addr, 0, $old['default_ship_via'] ? $old['default_ship_via'] : 1,
        $old['notes'] == '1' ? '' : $old['notes'], NULL);
    return db_fetch_assoc(db_query("SELECT * FROM ".TB_PREF."cust_branch WHERE branch_code = ".db_insert_id(),
        "could not load new branch"));
}

/**
 * The FA lines for one Medusa line: [sku, qty, FA list price, discount].
 * A kit variant (metadata.fa_kit, SKU "GRIT-06/PITCH-64") = the FA kit at its
 * price + the chosen pitch × slot qty, discounted so the two add up to what
 * the customer paid. Any other line: the FA list price, discounted down to
 * the price paid (as woo_orders).
 */
function medusa_fa_lines($it, $sales_type) {
    $qty = (float)$it['quantity'];
    $paid = (float)$it['unit_price'];
    $meta = isset($it['variant']['metadata']) ? (array)$it['variant']['metadata'] : array();
    if (!empty($meta['fa_kit'])) {
        $kit = $meta['fa_kit'];
        $parts = explode('/', $it['variant_sku']);
        $pitch = end($parts);
        $slot = 1;
        foreach ((array)$meta['parts'] as $p) if ($p['sku'] == $pitch) $slot = (float)$p['qty'];
        $kit_price = get_kit_price($kit, 'USD', $sales_type);
        $pitch_list = get_kit_price($pitch, 'USD', $sales_type);
        $pitch_paid = $paid - $kit_price;  // per kit, for $slot pitch units
        $disc = $pitch_list > 0 ? 1.0 - $pitch_paid / ($slot * $pitch_list) : 0;
        return array(
            array($kit, $qty, $kit_price, 0),
            array($pitch, $qty * $slot, $pitch_list, max(0, $disc)),
        );
    }
    $list = get_kit_price($it['variant_sku'], 'USD', $sales_type);
    if ($list <= $paid || $list == 0) return array(array($it['variant_sku'], $qty, $paid, 0));
    return array(array($it['variant_sku'], $qty, $list, 1.0 - $paid / $list));
}

/** Imports one Medusa order; returns a line of text for the screen. Throws nothing: errors come back as text. */
function medusa_import_order($o, $opts) {
    global $MEDUSA_REF, $Refs;
    $n = $o['display_id'];
    $a = !empty($o['shipping_address']['address_1']) ? $o['shipping_address'] : $o['billing_address'];
    $email = $o['email'];
    $name = trim($a['first_name'] . ' ' . $a['last_name']);
    $addr = medusa_address_format($a);
    $taxgid = medusa_tax_group($o, $opts['tax_group_id']);

    foreach ($o['items'] as $it) {
        if (fa_get_item_code_id_m($it['variant_sku']) === false && empty($it['variant']['metadata']['fa_kit']))
            return "Order #$n: SKU ".$it['variant_sku']." is not in FA, skipped";
    }

    begin_transaction();
    list($debtor_no, $is_new) = medusa_customer($email, $name, $addr, $taxgid, $opts);
    $branch = medusa_branch($debtor_no, $addr, $taxgid);
    if (!$branch) { cancel_transaction(); return "Order #$n: customer $debtor_no has no branch, skipped"; }
    $customer = db_fetch_assoc(db_query("SELECT * FROM ".TB_PREF."debtors_master WHERE debtor_no = "
        .db_escape($debtor_no), "debtor"));

    $order_date = sql2date(substr($o['created_at'], 0, 10));
    $cart = new Cart(ST_SALESORDER);
    $cart->set_customer($debtor_no, $customer['name'], $customer['curr_code'], 0, $customer['payment_terms']);
    $cart->cust_ref = $MEDUSA_REF . $n;
    $cart->Comments = '';
    $cart->document_date = $order_date;
    $cart->due_date = $order_date;
    $cart->sales_type = $customer['sales_type'];
    $cart->ship_via = $branch['default_ship_via'] ? $branch['default_ship_via'] : 1;
    $cart->deliver_to = $branch['br_name'];
    $cart->delivery_address = $branch['br_address'];
    $cart->set_branch($branch['branch_code'], $taxgid, '', isset($a['phone']) ? $a['phone'] : '', $email);
    $cart->freight_cost = round((float)$o['shipping_subtotal'], 2);
    $cart->Location = $branch['default_location'];
    foreach ($o['items'] as $it)
        foreach (medusa_fa_lines($it, $opts['sales_type']) as $l)
            add_to_order($cart, $l[0], $l[1], $l[2], $l[3]);
    $order_no = $cart->write(0);

    // One FA customer payment per captured Stripe payment, unallocated; fee = bank charge.
    $pays = array();
    foreach ($o['payment_collections'] as $pc) {
        foreach ((array)$pc['payments'] as $p) {
            if (empty($p['captured_at']) || !empty($p['canceled_at'])) continue;
            $amount = round((float)$p['amount'], 2);
            $charge = isset($p['data']['latest_charge']) ? $p['data']['latest_charge'] : '';
            $fee = medusa_stripe_fee($charge);
            $date = sql2date(substr($p['captured_at'], 0, 10));
            $ref = $Refs->get_next(ST_CUSTPAYMENT, null, array('customer' => $debtor_no,
                'branch' => $branch['branch_code'], 'date' => $date));
            write_customer_payment(0, $debtor_no, $branch['branch_code'], $opts['bank_account'], $date, $ref,
                $amount, 0, $MEDUSA_REF . "$n Stripe $charge", 0, $fee === null ? 0 : $fee);
            $pays[] = '$'.number_format($amount, 2) . ($fee === null ? ' (fee unknown)' : ' fee $'.number_format($fee, 2));
        }
    }
    commit_transaction();
    return "Order #$n -> FA sales order $order_no for $name" . ($is_new ? ' (new customer)' : '')
        . (count($pays) ? '; payment ' . implode(', ', $pays) : '; no captured payment');
}

function fa_get_item_code_id_m($sku) {
    $res = db_query("SELECT id FROM ".TB_PREF."item_codes WHERE item_code = ".db_escape($sku), "could not get item id");
    $row = db_fetch_row($res);
    return is_array($row) ? $row[0] : false;
}

<?php
/**********************************************
Author: Tom Moulton
Name: Medusa shop interface: price + stock push (FA is the master)
Free software under GNU GPLv3

Prices: every shop variant gets its FA Retail USD price; a kit variant
("GRIT-06/PITCH-64") = FA kit price + slot qty × pitch list × (1 − kit %),
kit % = 1 − FA kit price ÷ Σ FA part list (as the catalog seed did).

Stock: the shop tracks POOLS (bulk grit in oz, Gugolz in half kg, pads,
plates, AccuLap), not the jars. Pool qty = FA on hand (pool + anything built
from it, e.g. finished jars) − undelivered FA sales orders, all converted
through BOMs and foreign codes into pool units. Variant → pool links and the
kit parts list are rebuilt from the FA BOM too, so a BOM change in FA reaches
the shop on the next push.
***********************************************/

// Pools: shop inventory item SKU => FA units -> shop units (same table as
// gotgrit-shop tools/extract_catalog.py).
$MEDUSA_POOLS = array();
foreach (array('SIC60', 'SIC80', 'SIC120', 'SIC220', 'SIC320', 'SIC500', 'WAO5', 'WAO9', 'WAO12',
    'WAO15', 'WAO25', 'CEO', 'ROUGE-RED') as $p) $MEDUSA_POOLS[$p] = 16;     // lb -> oz
foreach (array('H55', 'H64', 'H73', 'H82') as $p) $MEDUSA_POOLS[$p] = 2;    // kg -> half kg
foreach (array('PADS', 'PLATE-6', 'PLATE-8', 'PLATE-10', 'PLATE-12',
    'HA00', 'HA09', 'HA18', 'HA30', 'HA39', 'HA60', 'HA63', 'HA78') as $p) $MEDUSA_POOLS[$p] = 1;

$MEDUSA_RETAIL = 1;   // FA sales type the shop sells at

/** FA BOMs and foreign codes, loaded once. */
function medusa_fa_structure() {
    static $s = null;
    if ($s !== null) return $s;
    $s = array('bom' => array(), 'foreign' => array());
    $res = db_query("SELECT parent, component, quantity FROM ".TB_PREF."bom ORDER BY parent, id", "bom");
    while ($r = db_fetch_assoc($res)) $s['bom'][$r['parent']][] = array($r['component'], (float)$r['quantity']);
    $res = db_query("SELECT item_code, stock_id, quantity FROM ".TB_PREF."item_codes WHERE is_foreign = 1 AND inactive = 0",
        "foreign codes");
    while ($r = db_fetch_assoc($res)) $s['foreign'][$r['item_code']] = array($r['stock_id'], (float)$r['quantity']);
    return $s;
}

/** Pool units in $qty (FA units) of an FA SKU: [pool => shop units]. */
function medusa_pool_units($sku, $qty = 1.0) {
    global $MEDUSA_POOLS;
    $s = medusa_fa_structure();
    if (isset($MEDUSA_POOLS[$sku])) return array($sku => $qty * $MEDUSA_POOLS[$sku]);
    if (isset($s['foreign'][$sku])) {
        list($base, $f) = $s['foreign'][$sku];
        return isset($MEDUSA_POOLS[$base]) ? array($base => $qty * $f * $MEDUSA_POOLS[$base]) : array();
    }
    $out = array();
    if (isset($s['bom'][$sku]))
        foreach ($s['bom'][$sku] as $c)
            foreach (medusa_pool_units($c[0], $qty * $c[1]) as $k => $v)
                $out[$k] = (isset($out[$k]) ? $out[$k] : 0) + $v;
    return $out;
}

/** Pool qty in shop units: FA on hand at $loc − undelivered sales orders. */
function medusa_pool_stock($loc) {
    global $MEDUSA_POOLS;
    $stock = array_fill_keys(array_keys($MEDUSA_POOLS), 0.0);
    $add = function ($sku, $qty) use (&$stock) {
        foreach (medusa_pool_units($sku, $qty) as $k => $v) $stock[$k] += $v;
    };
    $res = db_query("SELECT stock_id, SUM(qty) q FROM ".TB_PREF."stock_moves WHERE loc_code = ".db_escape($loc)
        ." GROUP BY stock_id HAVING q <> 0", "on hand");
    while ($r = db_fetch_assoc($res)) $add($r['stock_id'], (float)$r['q']);
    $res = db_query("SELECT d.stk_code, SUM(d.quantity - d.qty_sent) q FROM ".TB_PREF."sales_order_details d"
        ." JOIN ".TB_PREF."sales_orders o ON o.order_no = d.order_no AND o.trans_type = d.trans_type"
        ." WHERE d.trans_type = ".ST_SALESORDER." AND d.quantity > d.qty_sent GROUP BY d.stk_code", "undelivered");
    while ($r = db_fetch_assoc($res)) $add($r['stk_code'], -(float)$r['q']);
    return $stock;
}

function medusa_retail($sku) {
    global $MEDUSA_RETAIL;
    $res = db_query("SELECT price FROM ".TB_PREF."prices WHERE stock_id = ".db_escape($sku)
        ." AND sales_type_id = ".(int)$MEDUSA_RETAIL." AND curr_abrev = 'USD'", "price");
    $row = db_fetch_row($res);
    return is_array($row) ? (float)$row[0] : null;
}

/** All pages of an admin list. */
function medusa_list($path, $key, $fields, &$error) {
    $out = array();
    $offset = 0;
    do {
        list($status, $page, $err) = medusa_api('GET', $path, array('limit' => 100, 'offset' => $offset, 'fields' => $fields));
        if ($err != '') { $error = "Medusa $path: $err"; return $out; }
        $out = array_merge($out, $page[$key]);
        $offset += 100;
    } while ($offset < $page['count']);
    return $out;
}

function medusa_same_links($a, $b) {
    if (count($a) != count($b)) return false;
    foreach ($a as $k => $v) if (!isset($b[$k]) || abs($b[$k] - $v) > 0.0005) return false;
    return true;
}

function medusa_links_text($l) {
    $t = array();
    foreach ($l as $k => $v) $t[] = "$k ".round($v, 3);
    return count($t) ? implode(', ', $t) : 'none';
}

/**
 * Works out every change the push would make. Returns
 * [rows (for the screen), price/variant updates per product, link batches per product,
 * level updates, error text].
 */
function medusa_push_plan($loc) {
    global $MEDUSA_POOLS;
    $rows = array();
    $variants = array();  // product id => [variant update]
    $links = array();     // product id => [create/update/delete]
    $levels = array();
    $error = '';
    $s = medusa_fa_structure();

    $items = medusa_list('/admin/inventory-items', 'inventory_items',
        'id,sku,location_levels.id,location_levels.location_id,location_levels.stocked_quantity', $error);
    if ($error != '') return array($rows, $variants, $links, $levels, $error);
    $item_id = array();
    foreach ($items as $i) if ($i['sku'] != '') $item_id[$i['sku']] = $i['id'];
    foreach (array_keys($MEDUSA_POOLS) as $p)
        if (!isset($item_id[$p])) $rows[] = array('warning', $p, 'pool has no shop inventory item', '', '');

    // ---- stock ----
    $stock = medusa_pool_stock($loc);
    foreach ($items as $i) {
        if (!isset($MEDUSA_POOLS[$i['sku']])) { $rows[] = array('warning', $i['sku'], 'shop inventory item is not an FA pool', '', ''); continue; }
        // Whole shop units only (a 0.95 oz crumb can't be sold).
        $want = floor(round($stock[$i['sku']], 3));
        $send = max(0, $want);
        $lv = isset($i['location_levels'][0]) ? $i['location_levels'][0] : null;
        if (!$lv) { $rows[] = array('warning', $i['sku'], 'no stock location in the shop', '', ''); continue; }
        $have = (float)$lv['stocked_quantity'];
        if (abs($have - $send) > 0.0005) {
            $levels[] = array('inventory_item_id' => $i['id'], 'location_id' => $lv['location_id'], 'stocked_quantity' => $send);
            $rows[] = array('stock', $i['sku'], round($have, 3), $send, $want < 0 ? "FA is short: $want" : '');
        }
    }

    // ---- prices, links, kit parts ----
    $products = medusa_list('/admin/products', 'products',
        'id,title,metadata,variants.id,variants.sku,variants.manage_inventory,variants.metadata,'
        .'variants.prices.id,variants.prices.amount,variants.prices.currency_code,'
        .'variants.inventory_items.inventory_item_id,variants.inventory_items.required_quantity', $error);
    if ($error != '') return array($rows, $variants, $links, $levels, $error);
    $sku_of = array_flip($item_id);

    foreach ($products as $p) {
        foreach ($p['variants'] as $v) {
            $sku = $v['sku'];
            $meta = is_array($v['metadata']) ? $v['metadata'] : array();
            $upd = array();

            if (!empty($meta['fa_kit'])) {
                $kit = $meta['fa_kit'];
                $parts = explode('/', $sku);
                $pitch = end($parts);
                $slot = isset($p['metadata']['pitch_slot_qty']) ? (float)$p['metadata']['pitch_slot_qty'] : 1;
                $kit_price = medusa_retail($kit);
                $bom = isset($s['bom'][$kit]) ? $s['bom'][$kit] : array();
                $part_list = 0;
                foreach ($bom as $c) $part_list += (float)medusa_retail($c[0]) * $c[1];
                if ($kit_price === null || $part_list <= 0) { $rows[] = array('warning', $sku, "FA kit $kit has no price or BOM", '', ''); continue; }
                $pct = 1 - $kit_price / $part_list;
                $price = round($kit_price + $slot * (float)medusa_retail($pitch) * (1 - $pct), 2);
                $want_links = array();
                $want_parts = array();
                foreach (array_merge($bom, array(array($pitch, $slot))) as $c) {
                    $want_parts[] = array('sku' => $c[0], 'qty' => $c[1]);
                    foreach (medusa_pool_units($c[0], $c[1]) as $k => $q) $want_links[$k] = (isset($want_links[$k]) ? $want_links[$k] : 0) + $q;
                }
                $have_parts = isset($meta['parts']) ? $meta['parts'] : array();
                $hp = array(); foreach ($have_parts as $x) $hp[] = $x['sku'].' '.(float)$x['qty'];
                $wp = array(); foreach ($want_parts as $x) $wp[] = $x['sku'].' '.(float)$x['qty'];
                $have_pct = isset($meta['kit_pct']) ? (float)$meta['kit_pct'] : -1;
                if ($hp != $wp || abs($have_pct - round($pct, 6)) > 0.0000005) {
                    $upd['metadata'] = array_merge($meta, array('parts' => $want_parts, 'kit_pct' => round($pct, 6)));
                    $rows[] = array('kit parts', $sku, implode(', ', $hp), implode(', ', $wp),
                        $hp == $wp ? 'kit % '.$have_pct.' -> '.round($pct, 6) : '');
                }
            } else {
                $price = medusa_retail($sku);
                if ($price === null) { $rows[] = array('warning', $sku, 'no FA Retail price; shop price left alone', '', ''); }
                $want_links = medusa_pool_units($sku);
            }

            if ($price !== null) {
                $usd = null;
                foreach ($v['prices'] as $pr) if ($pr['currency_code'] == 'usd') $usd = $pr;
                if (!$usd || abs((float)$usd['amount'] - $price) > 0.004) {
                    $upd['prices'] = array(array('currency_code' => 'usd', 'amount' => $price) + ($usd ? array('id' => $usd['id']) : array()));
                    $rows[] = array('price', $sku, $usd ? number_format($usd['amount'], 2) : 'none', number_format($price, 2), '');
                }
            }

            // Links: pools only (an untracked part with no stock would show the item sold out).
            $have_links = array();
            foreach ((array)$v['inventory_items'] as $l)
                $have_links[isset($sku_of[$l['inventory_item_id']]) ? $sku_of[$l['inventory_item_id']] : $l['inventory_item_id']] = (float)$l['required_quantity'];
            $missing = array_diff(array_keys($want_links), array_keys($item_id));
            if (count($missing)) { $rows[] = array('warning', $sku, 'pool not in the shop: '.implode(', ', $missing), '', ''); continue; }
            if (!medusa_same_links($have_links, $want_links)) {
                foreach ($want_links as $k => $q) {
                    $op = isset($have_links[$k]) ? 'update' : 'create';
                    if ($op == 'update' && abs($have_links[$k] - $q) <= 0.0005) continue;
                    $links[$p['id']][$op][] = array('variant_id' => $v['id'], 'inventory_item_id' => $item_id[$k], 'required_quantity' => $q);
                }
                foreach ($have_links as $k => $q)
                    if (!isset($want_links[$k]))
                        $links[$p['id']]['delete'][] = array('variant_id' => $v['id'], 'inventory_item_id' => isset($item_id[$k]) ? $item_id[$k] : $k);
                $rows[] = array('links', $sku, medusa_links_text($have_links), medusa_links_text($want_links), '');
            }
            $track = !empty($meta['fa_kit']) || count($want_links) > 0;
            if ((bool)$v['manage_inventory'] != $track) {
                $upd['manage_inventory'] = $track;
                $rows[] = array('tracked', $sku, $v['manage_inventory'] ? 'yes' : 'no', $track ? 'yes' : 'no', '');
            }
            if (count($upd)) $variants[$p['id']][] = array('id' => $v['id']) + $upd;
        }
    }
    return array($rows, $variants, $links, $levels, $error);
}

/** Applies a plan; returns lines of text for the screen. */
function medusa_push_apply($variants, $links, $levels) {
    $log = array();
    $n = 0;
    foreach ($variants as $pid => $upd) {
        list($st, $r, $err) = medusa_api('POST', "/admin/products/$pid/variants/batch", array(), array('update' => $upd));
        if ($err != '') $log[] = "Product $pid variants: $err"; else $n += count($upd);
    }
    $log[] = "$n variant(s) updated";
    $n = 0;
    foreach ($links as $pid => $b) {
        list($st, $r, $err) = medusa_api('POST', "/admin/products/$pid/variants/inventory-items/batch", array(), $b);
        if ($err != '') $log[] = "Product $pid stock links: $err"; else $n++;
    }
    $log[] = "Stock links changed on $n product(s)";
    if (count($levels)) {
        list($st, $r, $err) = medusa_api('POST', '/admin/inventory-items/location-levels/batch', array(), array('update' => $levels));
        $log[] = $err != '' ? "Stock levels: $err" : count($levels)." stock level(s) set";
    } else $log[] = "Stock levels already match";
    return $log;
}

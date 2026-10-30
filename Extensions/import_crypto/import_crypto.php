<?php
/**********************************************
Author: Joe Hunt
Author: Tom Moulton - added Export of many types and import of the same
Name: Import of CSV formatted items
Free software under GNU GPL
***********************************************/
$page_security = 'SA_CSVIMPORT';
$path_to_root="../..";

include($path_to_root . "/includes/session.inc");
add_access_extensions();

include_once($path_to_root . "/includes/ui.inc");
include_once($path_to_root . "/includes/data_checks.inc");

include_once($path_to_root . "/inventory/includes/inventory_db.inc");
include_once($path_to_root . "/inventory/includes/db/items_codes_db.inc");
include_once($path_to_root . "/dimensions/includes/dimensions_db.inc");

/*
function get_add_workcenter($name) {
    $name = db_escape($name);
    $sql = "SELECT id FROM ".TB_PREF."workcentres WHERE UPPER( name ) = UPPER( $name )";
    $result = db_query($sql, "Can not search workcentres table");
    $row = db_fetch_row($result);
    if (!$row[0]) {
        $sql = "INSERT INTO ".TB_PREF."workcentres (name, description) VALUES ( $name, $name)";
        $result = db_query($sql, "Could not add workcenter");
        $id = db_insert_id();
        display_notification("Added $name as id $id");
    } else $id = $row[0];
    return $id;
}

function check_stock_id($stock_id) {
    $sql = "SELECT * FROM ".TB_PREF."stock_master where stock_id = $stock_id";
    $result = db_query($sql, "Can not look up stock_id");
    $row = db_fetch_row($result);
    if (!$row[0]) return 0;
    return 1;
}

function get_supplier_id($supplier) {
    $sql = "SELECT supplier_id FROM ".TB_PREF."suppliers where supp_name = ".db_escape($supplier);
    $result = db_query($sql, "Can not look up supplier");
    $row = db_fetch_row($result);
    if (!$row[0]) return 0;
    return $row[0];
}

function get_dimension_by_name($name) {
    if ($name == '') return 0;

    $sql = "SELECT * FROM ".TB_PREF."dimensions WHERE name=".db_escape($name);
    $result = db_query($sql, "Could not find dimension");
    if (db_num_rows($result) == 0) return -1;
    $row = db_fetch_row($result);
    if (!$row[0]) return -1;
    return $row[0];
}

function get_item_category_by_name($name)
{
        $sql="SELECT * FROM ".TB_PREF."stock_category WHERE description=".db_escape($name);
        $result = db_query($sql,"an item category could not be retrieved");
        return db_fetch($result);
}

function download_file($filename, $saveasname='')
{
    if (empty($filename) || !file_exists($filename))
    {
        return false;
    }
    if ($saveasname == '') $saveasname = basename($filename);
    header('Content-type: application/vnd.ms-excel');
    header('Content-Length: '.filesize($filename));
    header('Content-Disposition: attachment; filename='.db_escape($saveasname));
    readfile($filename);

    return true;
}

// change this from file to mysql $result
function download_csv($filename, $saveasname='')
{
    if (empty($filename) || !file_exists($filename))
    {
        return false;
    }
    if ($saveasname == '') $saveasname = basename($filename);
    header('Content-type: application/vnd.ms-excel');
    header('Content-Disposition: attachment; filename='.db_escape($saveasname));
// print all results, converting data as needed
    return true;
}
*/

// Column Names
$cnames = ['A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J'];

// Allowed actions
$actions = ['SKIP', 'Date', 'Epoch', 'Sent', 'Stype', 'Received', 'Rtype', 'Gas', 'Gtype'];

// Predefined templates
$templates = [
    'Manual' => array_fill(0, 10, 'SKIP'),
    'Cointracker' => ['Date', 'Sent', 'Received', 'Gas', 'SKIP', 'SKIP', 'Epoch', 'SKIP', 'Rtype', 'Gtype'],
    'Coinledger' => ['Date', 'Sent', 'Received', 'Gas', 'SKIP', 'SKIP', 'Epoch', 'SKIP', 'Rtype', 'Gtype'],
    'Koinly' => ['Date', 'Epoch', 'Sent', 'Stype', 'Received', 'Rtype', 'Gas', 'Gtype', 'SKIP', 'SKIP']
];

function displayForm($actions, $selectedColumns = []) {
    global $templates;
    
    for ($i = 0; $i < 10; $i++) {
        $ch = $cnames[$i];
        echo "Column $ch: <select name='columns[$i]' id='column_$i'>";
        foreach ($actions as $action) {
            $selected = isset($selectedColumns[$i]) && $selectedColumns[$i] === $action ? "selected" : "";
            echo "<option value='$action' $selected>$action</option>";
        }
        echo "</select><br>";
    }
}

// Validate column selections
function validateSelections($columnMappings) {
    $selectedActions = array_filter($columnMappings, fn($action) => $action !== 'SKIP');
    if (count($selectedActions) !== count(array_unique($selectedActions))) {
        return "Each action can only be selected once.";
    }
    if (in_array('Date', $selectedActions) && in_array('Epoch', $selectedActions)) {
        return "Date and Epoch cannot both be selected.";
    }
    return null;
}

// Process the uploaded CSV file
function processCSV($file, $columnMappings) {
    if (($handle = fopen($file, 'r')) !== FALSE) {
        while (($data = fgetcsv($handle, 1000, ",")) !== FALSE) {
            $formattedOutput = processRow($data, $columnMappings);
            echo "<pre>$formattedOutput</pre>";
        }
        fclose($handle);
    } else {
        echo "Error opening the file.";
    }
}

// Process each row of the CSV based on mappings
function processRow($data, $columnMappings) {
    $output = [];
    foreach ($columnMappings as $index => $action) {
        if ($action !== 'SKIP' && isset($data[$index])) {
            $output[] = "$action: " . htmlspecialchars($data[$index]);
        }
    }
    return implode(" | ", $output);
}

$csv_format = 'manual';
if (isset($_GET['csv_format'])) $csv_format = $_GET['csv_format'];
if (isset($_POST['csv_format'])) $csv_format = $_POST['csv_format'];

// Handle form submission
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    if (isset($_FILES['imp']) && $_FILES['imp']['name'] != '') {
        $filename = $_FILES['imp']['tmp_name'];
        $sep = $_POST['sep'];

        $columnMappings = $_POST['columns'] ?? [];
        $selectedTemplate = $_POST['template'] ?? 'Manual';
        
        $validationError = validateSelections($columnMappings);
        if ($validationError) {
            $validationResult = "<p style='color:red;'>$validationError</p>";
        } else {
            $validationResult = "<h2>Processed CSV Data:</h2>";
        }
    }
} else {
    $columnMappings = $templates[0];
    if ($csv_format == 'manual') $columnMappings = $templates[0];
    if ($csv_format == 'cointracker') $columnMappings = $templates[1];
    if ($csv_format == 'coinledger') $columnMappings = $templates[2];
    if ($csv_format == 'koinly') $columnMappings = $templates[3];
}


page("Import of CSV formatted Items");


echo 'Format: ';
if ($csv_format == 'manual') echo 'Manual';
else hyperlink_params($_SERVER['PHP_SELF'], _("Manual"), "csv_format=manual", false);
echo '&nbsp;|&nbsp;';
if ($csv_format == 'cointracker') echo 'Cointracker';
else hyperlink_params($_SERVER['PHP_SELF'], _("Cointracker"), "csv_format=cointracker", false);
echo '&nbsp;|&nbsp;';
if ($csv_format == 'coinledger') echo 'Coinledger';
else hyperlink_params($_SERVER['PHP_SELF'], _("Coinledger"), "csv_format=coinledger", false);
echo '&nbsp;|&nbsp;';
if ($csv_format == 'koinly') echo 'Koinly';
else hyperlink_params($_SERVER['PHP_SELF'], _("Koinly"), "csv_format=koinly", false);
echo "<br><br>";

if ($validationResult) echo $validationResult;

if ($action == 'import') {
    start_table(TABLESTYLE2, "width=60%");

    start_form(true);

    if (!isset($_POST['sep']))
        $_POST['sep'] = ",";

    table_section_title("Separator, Column definitions");
    text_row("Field separator:", 'sep', $_POST['sep'], 2, 1);
    displayForm($actions, $columnMappings);

    label_row("CSV Import File:", "<input type='file' id='imp' name='imp'>");

    end_table(1);

    submit_center('import', "Import CSV File");

    end_form();
}

end_page();

<?php

// Allowed actions
$actions = ['SKIP', 'Date', 'Epoch', 'Sent', 'Stype', 'Received', 'Rtype', 'Gas', 'Gtype'];

// Display the form
function displayForm($actions, $selectedColumns = []) {
    echo '<form method="post" enctype="multipart/form-data">';
    echo 'Upload CSV File: <input type="file" name="csv_file" required><br><br>';
    
    for ($i = 0; $i < 10; $i++) {
        echo "Column $i: <select name='columns[$i]'>";
        foreach ($actions as $action) {
            $selected = isset($selectedColumns[$i]) && $selectedColumns[$i] === $action ? "selected" : "";
            echo "<option value='$action' $selected>$action</option>";
        }
        echo "</select><br>";
    }
    
    echo '<br><input type="submit" name="upload" value="Upload and Process">';
    echo '</form>';
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

// Handle form submission
if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_FILES['csv_file'])) {
    $tmpName = $_FILES['csv_file']['tmp_name'];
    $columnMappings = $_POST['columns'] ?? [];
    
    $validationError = validateSelections($columnMappings);
    if ($validationError) {
        echo "<p style='color:red;'>$validationError</p>";
        displayForm($actions, $columnMappings);
    } else {
        echo "<h2>Processed CSV Data:</h2>";
        processCSV($tmpName, $columnMappings);
    }
} else {
    displayForm($actions);
}

?>


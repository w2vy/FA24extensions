<?php

// Allowed actions
$actions = ['SKIP', 'Date', 'Epoch', 'Sent', 'Stype', 'Received', 'Rtype', 'Gas', 'Gtype'];

// Predefined templates
$templates = [
    'Manual' => array_fill(0, 10, 'SKIP'),
    'CoinLedger' => ['Date', 'Sent', 'Received', 'Gas', 'SKIP', 'SKIP', 'Epoch', 'SKIP', 'Rtype', 'Gtype'],
    'Koinly' => ['Date', 'Epoch', 'Sent', 'Stype', 'Received', 'Rtype', 'Gas', 'Gtype', 'SKIP', 'SKIP']
];

// Display the form
function displayForm($actions, $selectedColumns = [], $selectedTemplate = 'Manual') {
    global $templates;
    echo '<form method="post" enctype="multipart/form-data">';
    echo 'Template Selection: <select name="template" id="template" onchange="updateColumns()">';
    
    foreach ($templates as $templateName => $templateValues) {
        $selected = ($selectedTemplate === $templateName) ? "selected" : "";
        echo "<option value='$templateName' $selected>$templateName</option>";
    }
    
    echo '</select><br><br>';
    echo 'Upload CSV File: <input type="file" name="csv_file" required><br><br>';
    
    for ($i = 0; $i < 10; $i++) {
        echo "Column $i: <select name='columns[$i]' id='column_$i'>";
        foreach ($actions as $action) {
            $selected = isset($selectedColumns[$i]) && $selectedColumns[$i] === $action ? "selected" : "";
            echo "<option value='$action' $selected>$action</option>";
        }
        echo "</select><br>";
    }
    
    echo '<br><input type="submit" name="upload" value="Upload and Process">';
    echo '</form>';
    echo '<script>
        const templates = ' . json_encode($templates) . ';
        function updateColumns() {
            let selectedTemplate = document.getElementById("template").value;
            let columns = templates[selectedTemplate];
            for (let i = 0; i < 10; i++) {
                document.getElementById("column_" + i).value = columns[i];
            }
        }
    </script>';
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
    $selectedTemplate = $_POST['template'] ?? 'Manual';
    
    $validationError = validateSelections($columnMappings);
    if ($validationError) {
        echo "<p style='color:red;'>$validationError</p>";
        displayForm($actions, $columnMappings, $selectedTemplate);
    } else {
        echo "<h2>Processed CSV Data:</h2>";
        processCSV($tmpName, $columnMappings);
    }
} else {
    displayForm($actions);
}

?>


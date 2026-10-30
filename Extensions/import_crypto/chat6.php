<?php

session_start(); // Start the session

// Allowed actions
$actions = ['SKIP', 'Date', 'Epoch', 'Sent', 'Stype', 'Received', 'Rtype', 'Gas', 'Gtype'];

// Predefined templates
$templates = [
    'Manual' => array_fill(0, 10, 'SKIP'),
    'CoinLedger' => ['Date', 'Sent', 'Received', 'Gas', 'SKIP', 'SKIP', 'Epoch', 'SKIP', 'Rtype', 'Gtype'],
    'Koinly' => ['Date', 'Epoch', 'Sent', 'Stype', 'Received', 'Rtype', 'Gas', 'Gtype', 'SKIP', 'SKIP']
];

// Display upload form
function displayUploadForm() {
    echo '<form method="post" enctype="multipart/form-data">';
    echo '<h2>Step 1: Upload CSV File</h2>';
    echo '<input type="file" name="csv_file" required><br><br>';
    echo '<input type="submit" name="upload_file" value="Upload and Preview">';
    echo '</form>';
}

// Display column mapping form
function displayMappingForm($actions, $dataPreview, $selectedColumns = [], $selectedTemplate = 'Manual', $filePath = '') {
    global $templates;
    echo '<h2>Step 2: Map Columns</h2>';
    echo '<form method="post">';
    echo '<input type="hidden" name="file_path" value="' . htmlspecialchars($filePath) . '">';
    echo 'Template Selection: <select name="template" id="template" onchange="updateColumns()">';
    
    foreach ($templates as $templateName => $templateValues) {
        $selected = ($selectedTemplate === $templateName) ? "selected" : "";
        echo "<option value='$templateName' $selected>$templateName</option>";
    }
    
    echo '</select><br><br>';
    echo '<table border="1"><tr>';
    
    for ($i = 0; $i < count($dataPreview[0]); $i++) {
        echo "<th>Column $i</th>";
    }
    echo "</tr><tr>";
    
    for ($i = 0; $i < count($dataPreview[0]); $i++) {
        echo "<td><select name='columns[$i]' id='column_$i'>";
        foreach ($actions as $action) {
            $selected = isset($selectedColumns[$i]) && $selectedColumns[$i] === $action ? "selected" : "";
            echo "<option value='$action' $selected>$action</option>";
        }
        echo "</select></td>";
    }
    echo "</tr><tr>";
    
    for ($i = 0; $i < count($dataPreview[0]); $i++) {
        echo "<td>" . htmlspecialchars($dataPreview[0][$i]) . "</td>";
    }
    echo "</tr></table>";
    
    echo '<br><input type="submit" name="process_data" value="Process Data">';
    echo '</form>';
}

// Handle file upload
if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_FILES['csv_file'])) {
    $tmpName = $_FILES['csv_file']['tmp_name'];
    $newLocation = 'uploads/temp_uploaded.csv';
    
    if (move_uploaded_file($tmpName, $newLocation)) {
        $_SESSION['uploaded_file'] = $newLocation;
        if (($handle = fopen($newLocation, 'r')) !== FALSE) {
            $dataPreview = [fgetcsv($handle, 1000, ",")];
            fclose($handle);
            displayMappingForm($actions, $dataPreview, [], 'Manual', $newLocation);
        } else {
            echo "Error opening the file.";
        }
    } else {
        echo "Failed to save uploaded file.";
    }
} elseif ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['process_data'])) {
    $columnMappings = $_POST['columns'] ?? [];
    $filePath = $_POST['file_path'] ?? '';
    
    if (!file_exists($filePath)) {
        echo "File not found. Please re-upload.";
        displayUploadForm();
        exit;
    }

    $validationError = validateSelections($columnMappings);
    if ($validationError) {
        echo "<p style='color:red;'>$validationError</p>";
        displayMappingForm($actions, [], $columnMappings, 'Manual', $filePath);
    } else {
        echo "<h2>Processed CSV Data:</h2>";
        processCSV($filePath, $columnMappings);
    }
} else {
    displayUploadForm();
}

?>


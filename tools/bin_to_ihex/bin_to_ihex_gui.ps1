Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$ErrorActionPreference = 'Stop'

function Resolve-PythonExe {
    $candidates = @(
        'd:\Tools\esp\python_env\idf5.2_py3.11_env\Scripts\python.exe',
        'python',
        'py'
    )

    foreach ($candidate in $candidates) {
        if ($candidate -like '*\*') {
            if (Test-Path $candidate) { return $candidate }
            continue
        }

        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }

    throw 'Python not found.'
}

function Invoke-BinToIhexTool {
    param(
        [string]$ScriptDir,
        [string]$InputBin,
        [string]$OutputHex,
        [string]$BaseAddr
    )

    $pythonExe = Resolve-PythonExe
    $toolPath = Join-Path $ScriptDir 'bin_to_ihex.py'

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $pythonExe
    $psi.WorkingDirectory = (Split-Path $ScriptDir -Parent)
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $reportPath = [System.IO.Path]::ChangeExtension($OutputHex, '.report.json')
    $args = @($toolPath, $InputBin, $OutputHex, '--base', $BaseAddr, '--report-json', $reportPath)
    $quotedArgs = foreach ($arg in $args) {
        if ($null -eq $arg) { continue }
        '"' + ([string]$arg).Replace('"', '\"') + '"'
    }
    $psi.Arguments = ($quotedArgs -join ' ')

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()
    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()

    if ($proc.ExitCode -ne 0) {
        if ($stderr) { throw ([string]$stderr).Trim() }
        if ($stdout) { throw ([string]$stdout).Trim() }
        throw "Tool failed with exit code $($proc.ExitCode)."
    }

    $text = ([string]($stdout + [Environment]::NewLine + $stderr)).Trim()
    $report = $null
    if (Test-Path -LiteralPath $reportPath) {
        $report = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 | ConvertFrom-Json
    }

    return @{
        Text = $text
        Report = $report
    }
}

function Get-TrimmedControlText {
    param($Control)
    if ($null -eq $Control) { return '' }
    if ($null -eq $Control.Text) { return '' }
    return ([string]$Control.Text).Trim()
}

function T {
    param([string]$Key, [string]$DefaultValue)
    if ($script:uiText -and $script:uiText.PSObject.Properties.Name -contains $Key) {
        return [string]$script:uiText.$Key
    }
    return $DefaultValue
}

function New-FileRow {
    param(
        [string]$LabelText,
        [int]$Top,
        [string]$BrowseMode,
        [string]$Filter
    )

    $label = New-Object System.Windows.Forms.Label
    $label.Text = $LabelText
    $label.Left = 10
    $label.Top = $Top + 4
    $label.Width = 160

    $text = New-Object System.Windows.Forms.TextBox
    $text.Left = 180
    $text.Top = $Top
    $text.Width = 560

    $button = New-Object System.Windows.Forms.Button
    $button.Text = T 'browse' 'Browse'
    $button.Left = 750
    $button.Top = $Top - 1
    $button.Width = 80
    $button.Tag = [pscustomobject]@{
        TextBox = $text
        Mode = $BrowseMode
        Filter = $Filter
    }

    $button.Add_Click({
        param($sender, $eventArgs)

        $ctx = $sender.Tag
        if ($null -eq $ctx -or $null -eq $ctx.TextBox) { return }
        $target = $ctx.TextBox
        $currentText = Get-TrimmedControlText $target

        if ($ctx.Mode -eq 'save') {
            $dialog = New-Object System.Windows.Forms.SaveFileDialog
            $dialog.Filter = $ctx.Filter
            if ($currentText) { $dialog.FileName = $currentText }
            if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
                $target.Text = $dialog.FileName
            }
            return
        }

        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Filter = $ctx.Filter
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            $target.Text = $dialog.FileName
        }
    })

    return @{
        Label = $label
        TextBox = $text
        Button = $button
    }
}

function Get-DefaultHexPath {
    param(
        [string]$BinPath,
        [string]$BaseAddr
    )

    if (-not $BinPath) { return '' }
    $srcPath = [System.IO.Path]::GetFullPath($BinPath)
    $srcDir = [System.IO.Path]::GetDirectoryName($srcPath)
    $srcStem = [System.IO.Path]::GetFileNameWithoutExtension($srcPath)
    $baseText = ([string]$BaseAddr).Trim()
    if (-not $baseText) {
        $baseText = '0x00000000'
    }
    $safeBase = $baseText.Replace('0x', '').Replace('0X', '').Replace(':', '_')
    return (Join-Path $srcDir ($srcStem + '_at_' + $safeBase + '.hex'))
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$langFile = Join-Path $scriptDir 'bin_to_ihex_gui_zh.json'
$script:uiText = $null
if (Test-Path $langFile) {
    $jsonText = [System.IO.File]::ReadAllText($langFile, [System.Text.Encoding]::UTF8)
    $script:uiText = $jsonText | ConvertFrom-Json
}

$form = New-Object System.Windows.Forms.Form
$form.Text = T 'app_title' 'BIN to HEX Tool'
$form.StartPosition = 'CenterScreen'
$form.Size = New-Object System.Drawing.Size(860, 500)
$form.MinimumSize = New-Object System.Drawing.Size(860, 500)
$form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::Font
$form.MaximizeBox = $true

$desc = New-Object System.Windows.Forms.Label
$desc.Left = 10
$desc.Top = 10
$desc.Width = 820
$desc.Height = 40
$desc.Text = T 'desc' 'Convert BIN to Intel HEX with base address.'
$desc.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor
               [System.Windows.Forms.AnchorStyles]::Left -bor
               [System.Windows.Forms.AnchorStyles]::Right
$form.Controls.Add($desc)

$rowBin = New-FileRow -LabelText (T 'input_bin' 'Input BIN') -Top 60 -BrowseMode 'open' -Filter (T 'filter_bin' 'BIN files (*.bin)|*.bin|All files (*.*)|*.*')
$form.Controls.Add($rowBin.Label)
$form.Controls.Add($rowBin.TextBox)
$form.Controls.Add($rowBin.Button)
$rowBin.Label.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor [System.Windows.Forms.AnchorStyles]::Left
$rowBin.TextBox.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor
                          [System.Windows.Forms.AnchorStyles]::Left -bor
                          [System.Windows.Forms.AnchorStyles]::Right
$rowBin.Button.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor [System.Windows.Forms.AnchorStyles]::Right

$rowHex = New-FileRow -LabelText (T 'output_hex' 'Output HEX') -Top 100 -BrowseMode 'save' -Filter (T 'filter_hex' 'HEX files (*.hex)|*.hex|All files (*.*)|*.*')
$form.Controls.Add($rowHex.Label)
$form.Controls.Add($rowHex.TextBox)
$form.Controls.Add($rowHex.Button)
$rowHex.Label.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor [System.Windows.Forms.AnchorStyles]::Left
$rowHex.TextBox.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor
                          [System.Windows.Forms.AnchorStyles]::Left -bor
                          [System.Windows.Forms.AnchorStyles]::Right
$rowHex.Button.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor [System.Windows.Forms.AnchorStyles]::Right

$baseLabel = New-Object System.Windows.Forms.Label
$baseLabel.Text = T 'base_addr' 'Base Address'
$baseLabel.Left = 10
$baseLabel.Top = 144
$baseLabel.Width = 160
$form.Controls.Add($baseLabel)

$baseText = New-Object System.Windows.Forms.TextBox
$baseText.Left = 180
$baseText.Top = 140
$baseText.Width = 180
$baseText.Text = '0x08003800'
$baseText.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor [System.Windows.Forms.AnchorStyles]::Left
$form.Controls.Add($baseText)

$btnCtr = New-Object System.Windows.Forms.Button
$btnCtr.Left = 380
$btnCtr.Top = 138
$btnCtr.Width = 120
$btnCtr.Text = T 'btn_ctr' 'CTR package'
$btnCtr.Add_Click({ $baseText.Text = '0x08003800' })
$btnCtr.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor [System.Windows.Forms.AnchorStyles]::Left
$form.Controls.Add($btnCtr)

$btnApp = New-Object System.Windows.Forms.Button
$btnApp.Left = 510
$btnApp.Top = 138
$btnApp.Width = 120
$btnApp.Text = T 'btn_app' 'Raw APP'
$btnApp.Add_Click({ $baseText.Text = '0x08004000' })
$btnApp.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor [System.Windows.Forms.AnchorStyles]::Left
$form.Controls.Add($btnApp)

$baseHint = New-Object System.Windows.Forms.Label
$baseHint.Left = 180
$baseHint.Top = 170
$baseHint.Width = 650
$baseHint.Text = T 'base_hint' 'Common base addresses'
$baseHint.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor
                   [System.Windows.Forms.AnchorStyles]::Left -bor
                   [System.Windows.Forms.AnchorStyles]::Right
$form.Controls.Add($baseHint)

$resultGroup = New-Object System.Windows.Forms.GroupBox
$resultGroup.Left = 10
$resultGroup.Top = 198
$resultGroup.Width = 820
$resultGroup.Height = 140
$resultGroup.Text = T 'result_group' 'Results'
$resultGroup.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor
                      [System.Windows.Forms.AnchorStyles]::Left -bor
                      [System.Windows.Forms.AnchorStyles]::Right
$form.Controls.Add($resultGroup)

$statusTitle = New-Object System.Windows.Forms.Label
$statusTitle.Left = 10
$statusTitle.Top = 26
$statusTitle.Width = 100
$statusTitle.Text = T 'status_title' 'Status'
$resultGroup.Controls.Add($statusTitle)

$statusBox = New-Object System.Windows.Forms.TextBox
$statusBox.Left = 170
$statusBox.Top = 22
$statusBox.Width = 180
$statusBox.ReadOnly = $true
$statusBox.Text = T 'status_wait' 'Pending'
$statusBox.BackColor = [System.Drawing.Color]::LightGray
$statusBox.ForeColor = [System.Drawing.Color]::Black
$resultGroup.Controls.Add($statusBox)

$kindLabel = New-Object System.Windows.Forms.Label
$kindLabel.Left = 380
$kindLabel.Top = 26
$kindLabel.Width = 120
$kindLabel.Text = T 'detected_kind' 'Detected'
$resultGroup.Controls.Add($kindLabel)

$kindValue = New-Object System.Windows.Forms.TextBox
$kindValue.Left = 500
$kindValue.Top = 22
$kindValue.Width = 290
$kindValue.ReadOnly = $true
$kindValue.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor
                    [System.Windows.Forms.AnchorStyles]::Left -bor
                    [System.Windows.Forms.AnchorStyles]::Right
$resultGroup.Controls.Add($kindValue)

$recommendedLabel = New-Object System.Windows.Forms.Label
$recommendedLabel.Left = 10
$recommendedLabel.Top = 60
$recommendedLabel.Width = 100
$recommendedLabel.Text = T 'recommended_base' 'Recommended Base'
$resultGroup.Controls.Add($recommendedLabel)

$recommendedValue = New-Object System.Windows.Forms.TextBox
$recommendedValue.Left = 170
$recommendedValue.Top = 56
$recommendedValue.Width = 180
$recommendedValue.ReadOnly = $true
$resultGroup.Controls.Add($recommendedValue)

$selectedLabel = New-Object System.Windows.Forms.Label
$selectedLabel.Left = 380
$selectedLabel.Top = 60
$selectedLabel.Width = 120
$selectedLabel.Text = T 'selected_base' 'Selected Base'
$resultGroup.Controls.Add($selectedLabel)

$selectedValue = New-Object System.Windows.Forms.TextBox
$selectedValue.Left = 500
$selectedValue.Top = 56
$selectedValue.Width = 290
$selectedValue.ReadOnly = $true
$resultGroup.Controls.Add($selectedValue)

$hexAddrLabel = New-Object System.Windows.Forms.Label
$hexAddrLabel.Left = 10
$hexAddrLabel.Top = 94
$hexAddrLabel.Width = 100
$hexAddrLabel.Text = T 'hex_first_addr' 'HEX First Addr'
$resultGroup.Controls.Add($hexAddrLabel)

$hexAddrValue = New-Object System.Windows.Forms.TextBox
$hexAddrValue.Left = 170
$hexAddrValue.Top = 90
$hexAddrValue.Width = 180
$hexAddrValue.ReadOnly = $true
$resultGroup.Controls.Add($hexAddrValue)

$vectorLabel = New-Object System.Windows.Forms.Label
$vectorLabel.Left = 380
$vectorLabel.Top = 94
$vectorLabel.Width = 120
$vectorLabel.Text = T 'vector_check' '0x800 Vector'
$resultGroup.Controls.Add($vectorLabel)

$vectorValue = New-Object System.Windows.Forms.TextBox
$vectorValue.Left = 500
$vectorValue.Top = 90
$vectorValue.Width = 290
$vectorValue.ReadOnly = $true
$vectorValue.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor
                      [System.Windows.Forms.AnchorStyles]::Left -bor
                      [System.Windows.Forms.AnchorStyles]::Right
$resultGroup.Controls.Add($vectorValue)

$headerLabel = New-Object System.Windows.Forms.Label
$headerLabel.Left = 10
$headerLabel.Top = 128
$headerLabel.Width = 100
$headerLabel.Text = T 'header_check' 'Header Check'
$resultGroup.Controls.Add($headerLabel)

$headerValue = New-Object System.Windows.Forms.TextBox
$headerValue.Left = 170
$headerValue.Top = 124
$headerValue.Width = 620
$headerValue.ReadOnly = $true
$headerValue.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor
                      [System.Windows.Forms.AnchorStyles]::Left -bor
                      [System.Windows.Forms.AnchorStyles]::Right
$resultGroup.Controls.Add($headerValue)

$runButton = New-Object System.Windows.Forms.Button
$runButton.Left = 710
$runButton.Top = 346
$runButton.Width = 120
$runButton.Height = 28
$runButton.Text = T 'btn_make' 'Generate HEX'
$runButton.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor [System.Windows.Forms.AnchorStyles]::Right
$form.Controls.Add($runButton)

$outputBox = New-Object System.Windows.Forms.TextBox
$outputBox.Left = 10
$outputBox.Top = 382
$outputBox.Width = 820
$outputBox.Height = 78
$outputBox.Multiline = $true
$outputBox.ReadOnly = $true
$outputBox.ScrollBars = 'Vertical'
$outputBox.Anchor = [System.Windows.Forms.AnchorStyles]::Top -bor
                    [System.Windows.Forms.AnchorStyles]::Bottom -bor
                    [System.Windows.Forms.AnchorStyles]::Left -bor
                    [System.Windows.Forms.AnchorStyles]::Right
$form.Controls.Add($outputBox)

$script:hexPathTouched = $false
$rowHex.TextBox.Add_TextChanged({
    if ($form.Tag -ne 'auto_fill_hex') {
        $script:hexPathTouched = $true
    }
})

$updateDefaultHex = {
    $binPath = Get-TrimmedControlText $rowBin.TextBox
    $baseAddr = Get-TrimmedControlText $baseText
    if (-not $script:hexPathTouched -and $binPath) {
        $form.Tag = 'auto_fill_hex'
        $rowHex.TextBox.Text = Get-DefaultHexPath -BinPath $binPath -BaseAddr $baseAddr
        $form.Tag = $null
    }
}

$rowBin.TextBox.Add_TextChanged($updateDefaultHex)
$baseText.Add_TextChanged($updateDefaultHex)

$runButton.Add_Click({
    try {
        $inputBin = Get-TrimmedControlText $rowBin.TextBox
        $outputHex = Get-TrimmedControlText $rowHex.TextBox
        $baseAddr = Get-TrimmedControlText $baseText

        if (-not $inputBin) { throw (T 'err_no_bin' 'Please choose input bin.') }
        if (-not $outputHex) { throw (T 'err_no_out' 'Please choose output hex.') }
        if (-not $baseAddr) { throw (T 'err_no_base' 'Please fill base address.') }

        $result = Invoke-BinToIhexTool -ScriptDir $scriptDir -InputBin $inputBin -OutputHex $outputHex -BaseAddr $baseAddr
        $outputBox.Text = $result.Text

        if ($null -ne $result.Report) {
            $report = $result.Report
            $kindValue.Text = [string]$report.input_bin.kind_label
            $recommendedValue.Text = [string]$report.input_bin.recommended_base_hex
            $selectedValue.Text = [string]$report.selected_base_hex
            $hexAddrValue.Text = [string]$report.output_hex.first_data_addr_hex
            if ([bool]$report.input_bin.has_2kb_ota_header) {
                $vectorValue.Text = (T 'vector_yes' 'Present') + " (offset $($report.input_bin.vector_offset_hex))"
                $headerSize = $report.input_bin.header_size
                $fileSize = $report.input_bin.size
                if (($headerSize + 0x800) -eq $fileSize) {
                    $headerValue.Text = (T 'header_ok' 'OK') + " ($headerSize + 0x800 = $fileSize)"
                    $headerValue.BackColor = [System.Drawing.Color]::LightGreen
                    $headerValue.ForeColor = [System.Drawing.Color]::DarkGreen
                } else {
                    $headerValue.Text = (T 'header_bad' 'BAD') + " ($headerSize + 0x800 != $fileSize)"
                    $headerValue.BackColor = [System.Drawing.Color]::LightCoral
                    $headerValue.ForeColor = [System.Drawing.Color]::DarkRed
                }
            } else {
                $vectorValue.Text = T 'vector_no' 'Absent'
                $headerValue.Text = T 'header_na' 'N/A'
                $headerValue.BackColor = [System.Drawing.Color]::White
                $headerValue.ForeColor = [System.Drawing.Color]::Black
            }

            if ([bool]$report.status_ok) {
                $statusBox.Text = T 'status_success' 'SUCCESS'
                $statusBox.BackColor = [System.Drawing.Color]::LightGreen
                $statusBox.ForeColor = [System.Drawing.Color]::DarkGreen
            } else {
                $statusBox.Text = T 'status_fail' 'FAIL'
                $statusBox.BackColor = [System.Drawing.Color]::LightCoral
                $statusBox.ForeColor = [System.Drawing.Color]::DarkRed
            }
        }

        [System.Windows.Forms.MessageBox]::Show((T 'done' 'Done'), (T 'app_title' 'BIN to HEX Tool'))
    } catch {
        $msg = $_.Exception.Message
        $outputBox.Text = $msg
        $statusBox.Text = T 'status_fail' 'FAIL'
        $statusBox.BackColor = [System.Drawing.Color]::LightCoral
        $statusBox.ForeColor = [System.Drawing.Color]::DarkRed
        $headerValue.BackColor = [System.Drawing.Color]::White
        $headerValue.ForeColor = [System.Drawing.Color]::Black
        [System.Windows.Forms.MessageBox]::Show($msg, (T 'app_title' 'BIN to HEX Tool'))
    }
})

[void]$form.ShowDialog()

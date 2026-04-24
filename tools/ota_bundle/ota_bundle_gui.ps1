Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$ErrorActionPreference = 'Stop'

function New-LabeledControlRow {
    param(
        [string]$LabelText,
        [int]$Top,
        [int]$LabelWidth = 170,
        [int]$TextLeft = 180,
        [int]$TextWidth = 560,
        [switch]$BrowseFile,
        [switch]$BrowseSave,
        [switch]$BrowseFolder
    )

    $label = New-Object System.Windows.Forms.Label
    $label.Text = $LabelText
    $label.Left = 10
    $label.Top = $Top + 4
    $label.Width = $LabelWidth

    $text = New-Object System.Windows.Forms.TextBox
    $text.Left = $TextLeft
    $text.Top = $Top
    $text.Width = $TextWidth

    $button = $null
    if ($BrowseFile -or $BrowseSave -or $BrowseFolder) {
        $button = New-Object System.Windows.Forms.Button
        $button.Text = 'Browse'
        $button.Left = $TextLeft + $TextWidth + 10
        $button.Top = $Top - 1
        $button.Width = 80
        $mode = 'file'
        if ($BrowseSave) { $mode = 'save' }
        if ($BrowseFolder) { $mode = 'folder' }
        $button.Tag = [pscustomobject]@{
            TextBox = $text
            Mode = $mode
        }

        $button.Add_Click({
            param($sender, $eventArgs)

            try {
                $ctx = $sender.Tag
                Write-DebugLog "browse click ctxNull=$($null -eq $ctx)"
                if ($null -eq $ctx) { return }
                $target = $ctx.TextBox
                Write-DebugLog "browse mode=$($ctx.Mode) targetNull=$($null -eq $target)"
                if ($null -eq $target) { return }
                $currentText = ''
                if ($null -ne $target.Text) { $currentText = [string]$target.Text }

                if ($ctx.Mode -eq 'folder') {
                    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
                    if ($currentText) { $dialog.SelectedPath = $currentText }
                    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
                        $target.Text = $dialog.SelectedPath
                        Write-DebugLog "browse selected folder=$($dialog.SelectedPath)"
                    }
                    return
                }

                if ($ctx.Mode -eq 'save') {
                    $dialog = New-Object System.Windows.Forms.SaveFileDialog
                    $dialog.Filter = 'BIN files (*.bin)|*.bin|JSON files (*.json)|*.json|All files (*.*)|*.*'
                    if ($currentText) { $dialog.FileName = $currentText }
                    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
                        $target.Text = $dialog.FileName
                        Write-DebugLog "browse selected save=$($dialog.FileName)"
                    }
                    return
                }

                $dialog = New-Object System.Windows.Forms.OpenFileDialog
                $dialog.Filter = 'All files (*.*)|*.*'
                if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
                    $target.Text = $dialog.FileName
                    Write-DebugLog "browse selected file=$($dialog.FileName)"
                }
            } catch {
                Write-DebugLog ("browse error type={0} msg={1}" -f $_.Exception.GetType().FullName, $_.Exception.Message)
                if ($_.InvocationInfo) {
                    Write-DebugLog ("browse line={0}" -f $_.InvocationInfo.PositionMessage)
                }
                throw
            }
        })
    }

    return @{
        Label = $label
        TextBox = $text
        Button = $button
    }
}

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

function Invoke-BundleTool {
    param(
        [string[]]$Arguments,
        [string]$ScriptDir
    )

    $pythonExe = Resolve-PythonExe
    $toolPath = Join-Path $ScriptDir 'pack_ota_bundle.py'

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $pythonExe
    $psi.WorkingDirectory = (Split-Path $ScriptDir -Parent)
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $allArgs = @($toolPath) + $Arguments
    $quotedArgs = foreach ($arg in $allArgs) {
        if ($null -eq $arg) { continue }
        '"' + ([string]$arg).Replace('"', '\"') + '"'
    }
    $psi.Arguments = ($quotedArgs -join ' ')
    Write-DebugLog ("Invoke-BundleTool exe={0}" -f $pythonExe)
    Write-DebugLog ("Invoke-BundleTool arguments={0}" -f $psi.Arguments)

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

    return ([string]($stdout + [Environment]::NewLine + $stderr)).Trim()
}

function Show-ToolError {
    param(
        [System.Windows.Forms.TextBox]$OutputBox,
        [string]$Message
    )

    $OutputBox.Text = $Message
    [System.Windows.Forms.MessageBox]::Show($Message, (T 'app_title' 'OTA Bundle Tool'))
}

function Write-DebugLog {
    param([string]$Message)

    try {
        if (-not $script:debugLogPath) { return }
        $ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'
        Add-Content -LiteralPath $script:debugLogPath -Value "[$ts] $Message" -Encoding UTF8
    } catch {
    }
}

function Get-TrimmedControlText {
    param($Control)

    if ($null -eq $Control) { return '' }
    if ($null -eq $Control.Text) { return '' }
    return ([string]$Control.Text).Trim()
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$langFile = Join-Path $scriptDir 'ota_bundle_gui_zh.json'
$uiText = $null
$toolBuildId = '2026-04-08.2'
$debugLogDir = Join-Path $scriptDir 'logs'
if (-not (Test-Path -LiteralPath $debugLogDir)) {
    New-Item -ItemType Directory -Path $debugLogDir -Force | Out-Null
}
$script:debugLogPath = Join-Path $debugLogDir 'ota_bundle_gui_debug.log'
if (Test-Path $langFile) {
    $jsonText = [System.IO.File]::ReadAllText($langFile, [System.Text.Encoding]::UTF8)
    $uiText = $jsonText | ConvertFrom-Json
}
Write-DebugLog "launch build=$toolBuildId"

function T {
    param(
        [string]$Key,
        [string]$DefaultValue
    )

    if ($uiText -and $uiText.PSObject.Properties.Name -contains $Key) {
        return [string]$uiText.$Key
    }
    return $DefaultValue
}

function Get-VersionFromFileName {
    param([string]$Path)

    if (-not $Path) { return '' }
    $name = [System.IO.Path]::GetFileNameWithoutExtension($Path)
    if (-not $name) { return '' }

    $patterns = @(
        '(?i)(?:^|[_\- ])v?(\d+\.\d+\.\d+(?:\.\d+)?)',
        '(?i)(?:^|[_\- ])v?(\d+\.\d+)',
        '(?i)(\d{4}_\d{2}_\d{2})'
    )

    foreach ($pattern in $patterns) {
        $m = [System.Text.RegularExpressions.Regex]::Match($name, $pattern)
        if ($m.Success) {
            return $m.Groups[1].Value
        }
    }
    return ''
}

function Get-SafeBundleNamePart {
    param(
        [string]$Value,
        [string]$Fallback
    )

    $text = ($Value | ForEach-Object { "$_" }).Trim()
    if (-not $text) {
        return $Fallback
    }

    $safe = [System.Text.RegularExpressions.Regex]::Replace($text, '[^A-Za-z0-9._-]+', '_')
    $safe = $safe.Trim('_')
    if (-not $safe) {
        return $Fallback
    }
    return $safe
}

function Get-DefaultBundlePath {
    param(
        [string]$CtrPath,
        [string]$CtrVersion,
        [string]$EspPath,
        [string]$EspVersion,
        [string]$ScriptDir
    )

    $toolsDir = Split-Path $ScriptDir -Parent
    $outDir = Join-Path $toolsDir 'after_package'
    if (-not (Test-Path $outDir)) {
        [void](New-Item -ItemType Directory -Path $outDir -Force)
    }

    $nameParts = @('ota')
    if ($CtrPath) {
        $nameParts += 'ctr'
        $nameParts += (Get-SafeBundleNamePart -Value $CtrVersion -Fallback 'version')
    }
    if ($EspPath) {
        $nameParts += 'hmi'
        $nameParts += (Get-SafeBundleNamePart -Value $EspVersion -Fallback 'version')
    }
    if ($nameParts.Count -eq 1) {
        $nameParts += 'bundle'
    }

    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $name = (($nameParts -join '_') + '_' + $stamp + '.bin')
    return (Join-Path $outDir $name)
}

$form = New-Object System.Windows.Forms.Form
$form.Text = "$(T 'app_title' 'OTA Bundle Tool') v$toolBuildId"
$form.Width = 980
$form.Height = 760
$form.StartPosition = 'CenterScreen'

$tabs = New-Object System.Windows.Forms.TabControl
$tabs.Dock = 'Fill'

$packPage = New-Object System.Windows.Forms.TabPage
$packPage.Text = T 'tab_pack' 'Pack'
$inspectPage = New-Object System.Windows.Forms.TabPage
$inspectPage.Text = T 'tab_inspect' 'Inspect'
$extractPage = New-Object System.Windows.Forms.TabPage
$extractPage.Text = T 'tab_extract' 'Extract'

$tabs.TabPages.Add($packPage)
$tabs.TabPages.Add($inspectPage)
$tabs.TabPages.Add($extractPage)
$form.Controls.Add($tabs)

# Pack tab
$packHint = New-Object System.Windows.Forms.Label
$packHint.Text = T 'pack_hint' 'For packaging/testing. Select at least one firmware. Only fields marked optional may be left empty.'
$packHint.Left = 10
$packHint.Top = 12
$packHint.Width = 930
$packHint.Height = 32

$packCtr = New-LabeledControlRow -LabelText (T 'pack_ctr_path' 'CTR firmware path') -Top 54 -BrowseFile
$packCtrVer = New-LabeledControlRow -LabelText (T 'pack_ctr_version' 'CTR version (optional)') -Top 90
$packEsp = New-LabeledControlRow -LabelText (T 'pack_esp_path' 'ESP32 firmware path') -Top 138 -BrowseFile
$packEspVer = New-LabeledControlRow -LabelText (T 'pack_esp_version' 'ESP32 version (optional)') -Top 174
$packPkgVer = New-LabeledControlRow -LabelText (T 'pack_bundle_version' 'Bundle version (optional)') -Top 222
$packOut = New-LabeledControlRow -LabelText (T 'pack_output_bundle' 'Output bundle path') -Top 258 -BrowseSave
$packInfo = New-LabeledControlRow -LabelText (T 'pack_output_info' 'Output info json (optional)') -Top 294 -BrowseSave

$packRun = New-Object System.Windows.Forms.Button
$packRun.Text = T 'pack_run' 'Pack'
$packRun.Left = 760
$packRun.Top = 338
$packRun.Width = 110

$packOutput = New-Object System.Windows.Forms.TextBox
$packOutput.Left = 10
$packOutput.Top = 378
$packOutput.Width = 920
$packOutput.Height = 322
$packOutput.Multiline = $true
$packOutput.ScrollBars = 'Both'
$packOutput.Font = New-Object System.Drawing.Font('Consolas', 9)

$packPage.Controls.Add($packHint)
foreach ($row in @($packCtr, $packCtrVer, $packEsp, $packEspVer, $packPkgVer, $packOut, $packInfo)) {
    $packPage.Controls.Add($row.Label)
    $packPage.Controls.Add($row.TextBox)
    if ($row.Button) { $packPage.Controls.Add($row.Button) }
}
$packPage.Controls.Add($packRun)
$packPage.Controls.Add($packOutput)

$script:packCtrTextBox = $packCtr.TextBox
$script:packCtrVerTextBox = $packCtrVer.TextBox
$script:packEspTextBox = $packEsp.TextBox
$script:packEspVerTextBox = $packEspVer.TextBox
$script:packOutTextBox = $packOut.TextBox

$script:packOutputInternalUpdate = $false
$script:packOutputUserEdited = $false

$packOut.TextBox.Add_TextChanged({
    try {
        if (-not $script:packOutputInternalUpdate) {
            $script:packOutputUserEdited = $true
            Write-DebugLog "packOut changed by user"
        }
    } catch {
        Write-DebugLog ("packOut TextChanged error={0}" -f $_.Exception.Message)
    }
})

function Update-PackDefaults {
    try {
        Write-DebugLog ("Update-PackDefaults enter ctrNull={0} ctrVerNull={1} espNull={2} espVerNull={3} outNull={4}" -f `
            ($null -eq $script:packCtrTextBox), ($null -eq $script:packCtrVerTextBox), ($null -eq $script:packEspTextBox), ($null -eq $script:packEspVerTextBox), ($null -eq $script:packOutTextBox))

        $ctrPath = Get-TrimmedControlText $script:packCtrTextBox
        $espPath = Get-TrimmedControlText $script:packEspTextBox
        Write-DebugLog "Update-PackDefaults paths ctr=$ctrPath esp=$espPath"

        if ($ctrPath -and -not (Get-TrimmedControlText $script:packCtrVerTextBox)) {
            $guess = Get-VersionFromFileName -Path $ctrPath
            Write-DebugLog "CTR version guess=$guess"
            if ($guess) {
                $script:packCtrVerTextBox.Text = $guess
            }
        }

        if ($espPath -and -not (Get-TrimmedControlText $script:packEspVerTextBox)) {
            $guess = Get-VersionFromFileName -Path $espPath
            Write-DebugLog "ESP version guess=$guess"
            if ($guess) {
                $script:packEspVerTextBox.Text = $guess
            }
        }

        if (-not $script:packOutputUserEdited) {
            $newPath = Get-DefaultBundlePath `
                -CtrPath $ctrPath `
                -CtrVersion (Get-TrimmedControlText $script:packCtrVerTextBox) `
                -EspPath $espPath `
                -EspVersion (Get-TrimmedControlText $script:packEspVerTextBox) `
                -ScriptDir $scriptDir
            Write-DebugLog "default bundle path=$newPath"
            $script:packOutputInternalUpdate = $true
            $script:packOutTextBox.Text = $newPath
            $script:packOutputInternalUpdate = $false
        }
        Write-DebugLog "Update-PackDefaults leave ok"
    } catch {
        Write-DebugLog ("Update-PackDefaults error type={0} msg={1}" -f $_.Exception.GetType().FullName, $_.Exception.Message)
        if ($_.InvocationInfo) {
            Write-DebugLog ("Update-PackDefaults line={0}" -f $_.InvocationInfo.PositionMessage)
        }
        throw
    }
}

function Invoke-UpdatePackDefaultsSafe {
    try {
        Write-DebugLog "Invoke-UpdatePackDefaultsSafe"
        Update-PackDefaults
    } catch {
        $packOutput.Text = "DEBUG: $($_.Exception.Message)`r`nLog: $script:debugLogPath"
        Write-DebugLog ("Invoke-UpdatePackDefaultsSafe error={0}" -f $_.Exception.Message)
    }
}

$packCtr.TextBox.Add_TextChanged({
    Write-DebugLog "packCtr TextChanged"
    Invoke-UpdatePackDefaultsSafe
})
$packEsp.TextBox.Add_TextChanged({
    Write-DebugLog "packEsp TextChanged"
    Invoke-UpdatePackDefaultsSafe
})

$packRun.Add_Click({
    try {
        Write-DebugLog "packRun click start"
        if (-not (Get-TrimmedControlText $packOut.TextBox)) {
            throw (T 'err_pack_output_required' 'Output bundle path is required.')
        }
        if ((-not (Get-TrimmedControlText $packCtr.TextBox)) -and (-not (Get-TrimmedControlText $packEsp.TextBox))) {
            throw (T 'err_pack_payload_required' 'Select at least one firmware: CTR or ESP32.')
        }

        $outPath = Get-TrimmedControlText $packOut.TextBox
        $ctrPath = Get-TrimmedControlText $packCtr.TextBox
        $ctrVer = Get-TrimmedControlText $packCtrVer.TextBox
        $espPath = Get-TrimmedControlText $packEsp.TextBox
        $espVer = Get-TrimmedControlText $packEspVer.TextBox
        $pkgVer = Get-TrimmedControlText $packPkgVer.TextBox
        $infoJson = Get-TrimmedControlText $packInfo.TextBox
        Write-DebugLog "packRun values ctr=$ctrPath ctrVer=$ctrVer esp=$espPath espVer=$espVer out=$outPath info=$infoJson"

        $args = @('pack', '--out', $outPath, '--align', '4096')
        if ($ctrPath) { $args += @('--ctr', $ctrPath) }
        if ($ctrVer) { $args += @('--ctr-version', $ctrVer) }
        if ($espPath) { $args += @('--esp32', $espPath) }
        if ($espVer) { $args += @('--esp32-version', $espVer) }
        if ($pkgVer) { $args += @('--package-version', $pkgVer) }
        if ($infoJson) { $args += @('--info-json', $infoJson) }
        Write-DebugLog ("packRun args={0}" -f ($args -join ' | '))

        $packOutput.Text = Invoke-BundleTool -Arguments $args -ScriptDir $scriptDir
        Write-DebugLog "packRun done ok"
        [System.Windows.Forms.MessageBox]::Show((T 'msg_pack_done' 'Pack done.'), (T 'app_title' 'OTA Bundle Tool'))
    } catch {
        Write-DebugLog ("packRun error type={0} msg={1}" -f $_.Exception.GetType().FullName, $_.Exception.Message)
        if ($_.InvocationInfo) {
            Write-DebugLog ("packRun line={0}" -f $_.InvocationInfo.PositionMessage)
        }
        Show-ToolError -OutputBox $packOutput -Message $_.Exception.Message
    }
})

# Inspect tab
$inspectHint = New-Object System.Windows.Forms.Label
$inspectHint.Text = T 'inspect_hint' 'For packaging, IOT and testers. Use this page to confirm bundle content, versions and offsets.'
$inspectHint.Left = 10
$inspectHint.Top = 12
$inspectHint.Width = 930
$inspectHint.Height = 32

$inspectBundle = New-LabeledControlRow -LabelText (T 'inspect_bundle_path' 'Bundle file path') -Top 54 -BrowseFile
$inspectRun = New-Object System.Windows.Forms.Button
$inspectRun.Text = T 'inspect_run' 'Inspect'
$inspectRun.Left = 780
$inspectRun.Top = 90
$inspectRun.Width = 120

$inspectOutput = New-Object System.Windows.Forms.TextBox
$inspectOutput.Left = 10
$inspectOutput.Top = 132
$inspectOutput.Width = 920
$inspectOutput.Height = 568
$inspectOutput.Multiline = $true
$inspectOutput.ScrollBars = 'Both'
$inspectOutput.Font = New-Object System.Drawing.Font('Consolas', 9)

$inspectPage.Controls.Add($inspectHint)
$inspectPage.Controls.Add($inspectBundle.Label)
$inspectPage.Controls.Add($inspectBundle.TextBox)
if ($inspectBundle.Button) { $inspectPage.Controls.Add($inspectBundle.Button) }
$inspectPage.Controls.Add($inspectRun)
$inspectPage.Controls.Add($inspectOutput)

$inspectRun.Add_Click({
    try {
        if (-not (Get-TrimmedControlText $inspectBundle.TextBox)) {
            throw (T 'err_inspect_bundle_required' 'Bundle file path is required.')
        }
        $inspectOutput.Text = Invoke-BundleTool -Arguments @('inspect', (Get-TrimmedControlText $inspectBundle.TextBox)) -ScriptDir $scriptDir
    } catch {
        Show-ToolError -OutputBox $inspectOutput -Message $_.Exception.Message
    }
})

# Extract tab
$extractHint = New-Object System.Windows.Forms.Label
$extractHint.Text = T 'extract_hint' 'For troubleshooting. Extract payloads from a bundle and compare them with the original firmware files.'
$extractHint.Left = 10
$extractHint.Top = 12
$extractHint.Width = 930
$extractHint.Height = 32

$extractBundle = New-LabeledControlRow -LabelText (T 'extract_bundle_path' 'Bundle file path') -Top 54 -BrowseFile
$extractDir = New-LabeledControlRow -LabelText (T 'extract_output_dir' 'Extract output folder') -Top 90 -BrowseFolder
$extractRun = New-Object System.Windows.Forms.Button
$extractRun.Text = T 'extract_run' 'Extract'
$extractRun.Left = 770
$extractRun.Top = 126
$extractRun.Width = 130

$extractOutput = New-Object System.Windows.Forms.TextBox
$extractOutput.Left = 10
$extractOutput.Top = 168
$extractOutput.Width = 920
$extractOutput.Height = 532
$extractOutput.Multiline = $true
$extractOutput.ScrollBars = 'Both'
$extractOutput.Font = New-Object System.Drawing.Font('Consolas', 9)

$extractPage.Controls.Add($extractHint)
$extractPage.Controls.Add($extractBundle.Label)
$extractPage.Controls.Add($extractBundle.TextBox)
if ($extractBundle.Button) { $extractPage.Controls.Add($extractBundle.Button) }
$extractPage.Controls.Add($extractDir.Label)
$extractPage.Controls.Add($extractDir.TextBox)
if ($extractDir.Button) { $extractPage.Controls.Add($extractDir.Button) }
$extractPage.Controls.Add($extractRun)
$extractPage.Controls.Add($extractOutput)

$extractRun.Add_Click({
    try {
        if (-not (Get-TrimmedControlText $extractBundle.TextBox)) {
            throw (T 'err_extract_bundle_required' 'Bundle file path is required.')
        }
        if (-not (Get-TrimmedControlText $extractDir.TextBox)) {
            throw (T 'err_extract_dir_required' 'Extract output folder is required.')
        }
        $extractOutput.Text = Invoke-BundleTool -Arguments @('extract', (Get-TrimmedControlText $extractBundle.TextBox), '--out-dir', (Get-TrimmedControlText $extractDir.TextBox)) -ScriptDir $scriptDir
        [System.Windows.Forms.MessageBox]::Show((T 'msg_extract_done' 'Extract done.'), (T 'app_title' 'OTA Bundle Tool'))
    } catch {
        Show-ToolError -OutputBox $extractOutput -Message $_.Exception.Message
    }
})

[void]$form.ShowDialog()

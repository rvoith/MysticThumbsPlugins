Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = $ScriptRoot
$ExternalRoot = Join-Path $RepoRoot 'External'
$ExternalSrcRoot = Join-Path $ExternalRoot 'src'
$ResvgRepoRoot = Join-Path $ExternalSrcRoot 'resvg'
$ResvgInstallRoot = Join-Path $ExternalRoot 'resvg\resvg-capi'
$VcpkgRoot = Join-Path $ExternalRoot 'vcpkg'
$PropsPath = Join-Path $RepoRoot 'MysticThumbs.ExternalDeps.props'
$LogPath = Join-Path $RepoRoot 'install_resvg_and_ffmpeg.log'

Set-Content -Path $LogPath -Value '' -Encoding utf8

function Write-Log {
    param(
        [Parameter(Mandatory = $true)][string]$Message,
        [ConsoleColor]$Color = 'White'
    )
    $timestamp = Get-Date -Format 'HH:mm:ss'
    $line = "[$timestamp] $Message"
    Write-Host $line -ForegroundColor $Color
    $line | Out-File -FilePath $LogPath -Append -Encoding utf8
}

function Test-IsAdmin {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [System.Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
}


function Get-RelativePathCompat {
    param(
        [Parameter(Mandatory = $true)][string]$BasePath,
        [Parameter(Mandatory = $true)][string]$TargetPath
    )

    $baseFull = [System.IO.Path]::GetFullPath($BasePath)
    $targetFull = [System.IO.Path]::GetFullPath($TargetPath)

    if (-not $baseFull.EndsWith('\')) {
        $baseFull += '\'
    }

    $baseUri = New-Object System.Uri($baseFull)
    $targetUri = New-Object System.Uri($targetFull)
    $relativeUri = $baseUri.MakeRelativeUri($targetUri)
    $relativePath = [System.Uri]::UnescapeDataString($relativeUri.ToString())

    return $relativePath.Replace('/', '\')
}


function Get-ExpectedProjectPath {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    $projectPath = Join-Path $RepoRoot $RelativePath
    if (Test-Path -LiteralPath $projectPath) {
        return $projectPath
    }
    return $null
}

function Ensure-Directory {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

function Assert-Command {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$HelpText = ''
    )
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        if ($HelpText) {
            throw "$Name was not found. $HelpText"
        }
        throw "$Name was not found on PATH."
    }
}

function Invoke-External {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$WorkingDirectory = $RepoRoot,
        [switch]$AllowNonZeroExit
    )

    $argString = (($ArgumentList | ForEach-Object {
        $arg = [string]$_
        if ($arg -match '[\s"]') {
            '"' + ($arg -replace '"', '\"') + '"'
        }
        else {
            $arg
        }
    }) -join ' ')

    Write-Log ("RUN: {0} {1}" -f $FilePath, $argString) DarkGray

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.Arguments = $argString
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()

    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()

    if ($stdout) {
        $stdout -split "`r?`n" | ForEach-Object {
            if ($_ -ne '') { Write-Log $_ Gray }
        }
    }
    if ($stderr) {
        $stderr -split "`r?`n" | ForEach-Object {
            if ($_ -ne '') { Write-Log $_ Yellow }
        }
    }

    if (-not $AllowNonZeroExit -and $proc.ExitCode -ne 0) {
        throw "Command failed with exit code $($proc.ExitCode): $FilePath $argString"
    }
    return $proc.ExitCode
}

function Ensure-GitCloneOrPull {
    param(
        [Parameter(Mandatory = $true)][string]$RepoUrl,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    if (Test-Path -LiteralPath (Join-Path $DestinationPath '.git')) {
        Write-Log "Updating existing repository: $DestinationPath" Cyan
        Invoke-External -FilePath 'git' -ArgumentList @('-C', $DestinationPath, 'pull', '--ff-only')
    }
    else {
        Ensure-Directory -Path (Split-Path -Parent $DestinationPath)
        Write-Log "Cloning repository: $RepoUrl" Cyan
        Invoke-External -FilePath 'git' -ArgumentList @('clone', $RepoUrl, $DestinationPath)
    }
}

function Ensure-RustToolchain {
    $cargoCmd = Get-Command cargo -ErrorAction SilentlyContinue
    if (-not $cargoCmd) {
        $rustupExe = Join-Path $ExternalRoot 'rustup-init.exe'
        if (-not (Test-Path -LiteralPath $rustupExe)) {
            Write-Log 'Downloading rustup-init.exe...' Cyan
            Invoke-WebRequest -Uri 'https://win.rustup.rs/x86_64' -OutFile $rustupExe
        }

        Write-Log 'Installing Rust MSVC toolchain...' Cyan
        Invoke-External -FilePath $rustupExe -ArgumentList @('-y', '--default-toolchain', 'stable') -WorkingDirectory $ExternalRoot

        $cargoBin = Join-Path $env:USERPROFILE '.cargo\bin'
        if (Test-Path -LiteralPath $cargoBin) {
            $pathParts = $env:PATH -split ';'
            if (-not ($pathParts -contains $cargoBin)) {
                $env:PATH = "$cargoBin;$env:PATH"
            }
        }
    }

    Assert-Command -Name 'rustup' -HelpText 'Rust installation did not complete correctly.'
    Assert-Command -Name 'cargo' -HelpText 'Cargo was not found after Rust installation.'

    Write-Log 'Configuring Rust targets...' Cyan
    Invoke-External -FilePath 'rustup' -ArgumentList @('default', 'stable')
    Invoke-External -FilePath 'rustup' -ArgumentList @('target', 'add', 'x86_64-pc-windows-msvc')
    Invoke-External -FilePath 'rustup' -ArgumentList @('target', 'add', 'i686-pc-windows-msvc')

    $cargoCbuild = Get-Command cargo-cbuild -ErrorAction SilentlyContinue
    if (-not $cargoCbuild) {
        Write-Log 'Installing cargo-c...' Cyan
        Invoke-External -FilePath 'cargo' -ArgumentList @('install', 'cargo-c', '--locked')
    }
    else {
        Write-Log 'cargo-c already installed.' DarkCyan
    }
}

function Build-Resvg {
    param(
        [Parameter(Mandatory = $true)][string]$Target,
        [Parameter(Mandatory = $true)][string]$Prefix
    )

    Ensure-Directory -Path $Prefix
    Write-Log "Building resvg C API for $Target -> $Prefix" Cyan

    $env:RUSTFLAGS = '-C target-feature=+crt-static'
    try {
        Invoke-External -FilePath 'cargo' `
            -WorkingDirectory $ResvgRepoRoot `
            -ArgumentList @(
                'cinstall',
                '--release',
                '--locked',
                '--manifest-path', 'crates/c-api/Cargo.toml',
                '--target', $Target,
                '--prefix', $Prefix
            )
    }
    finally {
        Remove-Item Env:RUSTFLAGS -ErrorAction SilentlyContinue
    }
}

function Ensure-Vcpkg {
    if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
        Ensure-GitCloneOrPull -RepoUrl 'https://github.com/microsoft/vcpkg.git' -DestinationPath $VcpkgRoot
        Write-Log 'Bootstrapping vcpkg...' Cyan
        Invoke-External -FilePath (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') -WorkingDirectory $VcpkgRoot
    }
    else {
        Ensure-GitCloneOrPull -RepoUrl 'https://github.com/microsoft/vcpkg.git' -DestinationPath $VcpkgRoot
        Write-Log 'Re-bootstrapping vcpkg...' Cyan
        Invoke-External -FilePath (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') -WorkingDirectory $VcpkgRoot
    }

    $vcpkgExe = Join-Path $VcpkgRoot 'vcpkg.exe'

    Write-Log 'Installing FFmpeg (x64 + dav1d)...' Cyan
    Invoke-External -FilePath $vcpkgExe -WorkingDirectory $VcpkgRoot -ArgumentList @('install', 'ffmpeg[dav1d]:x64-windows')

    Write-Log 'Installing FFmpeg (x86 + dav1d)...' Cyan
    Invoke-External -FilePath $vcpkgExe -WorkingDirectory $VcpkgRoot -ArgumentList @('install', 'ffmpeg[dav1d]:x86-windows', '--allow-unsupported')
}

function Write-ExternalDepsProps {
    $props = @'
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <MysticThumbsExternalRoot>$(MSBuildThisFileDirectory)External</MysticThumbsExternalRoot>
  </PropertyGroup>

  <PropertyGroup Condition="'$(Platform)'=='Win32'">
    <MysticThumbsResvgInclude>$(MysticThumbsExternalRoot)\resvg\resvg-capi\x86\include</MysticThumbsResvgInclude>
    <MysticThumbsResvgLib>$(MysticThumbsExternalRoot)\resvg\resvg-capi\x86\lib</MysticThumbsResvgLib>
    <MysticThumbsFFmpegInclude>$(MysticThumbsExternalRoot)\vcpkg\installed\x86-windows\include</MysticThumbsFFmpegInclude>
    <MysticThumbsFFmpegLib>$(MysticThumbsExternalRoot)\vcpkg\installed\x86-windows\lib</MysticThumbsFFmpegLib>
  </PropertyGroup>

  <PropertyGroup Condition="'$(Platform)'=='x64'">
    <MysticThumbsResvgInclude>$(MysticThumbsExternalRoot)\resvg\resvg-capi\x64\include</MysticThumbsResvgInclude>
    <MysticThumbsResvgLib>$(MysticThumbsExternalRoot)\resvg\resvg-capi\x64\lib</MysticThumbsResvgLib>
    <MysticThumbsFFmpegInclude>$(MysticThumbsExternalRoot)\vcpkg\installed\x64-windows\include</MysticThumbsFFmpegInclude>
    <MysticThumbsFFmpegLib>$(MysticThumbsExternalRoot)\vcpkg\installed\x64-windows\lib</MysticThumbsFFmpegLib>
  </PropertyGroup>
</Project>
'@
    Set-Content -Path $PropsPath -Value $props -Encoding utf8
    Write-Log "Wrote props file: $PropsPath" Green
}


function Get-MsbuildXmlDocument {
    param([Parameter(Mandatory = $true)][string]$ProjectPath)

    $xml = New-Object System.Xml.XmlDocument
    $xml.PreserveWhitespace = $true
    $xml.Load($ProjectPath)

    $nsUri = $xml.DocumentElement.NamespaceURI
    $nsmgr = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
    $nsmgr.AddNamespace('msb', $nsUri)

    return @{ Xml = $xml; Ns = $nsmgr; NsUri = $nsUri }
}

function Ensure-PropsImport {
    param([Parameter(Mandatory = $true)][string]$ProjectPath)

    $ctx = Get-MsbuildXmlDocument -ProjectPath $ProjectPath
    $xml = $ctx.Xml
    $nsmgr = $ctx.Ns
    $nsUri = $ctx.NsUri

    $relativeProps = Get-RelativePathCompat -BasePath (Split-Path -Parent $ProjectPath) -TargetPath $PropsPath
    $xpath = "//msb:Import[contains(@Project, 'MysticThumbs.ExternalDeps.props')]"
    $existing = $xml.SelectSingleNode($xpath, $nsmgr)
    if ($existing) {
        Write-Log "Props import already present in $(Split-Path -Leaf $ProjectPath)" DarkCyan
        return
    }

    $groups = $xml.SelectNodes("//msb:ImportGroup[@Label='PropertySheets']", $nsmgr)
    if (-not $groups -or $groups.Count -eq 0) {
        throw "Could not find any PropertySheets import groups in $ProjectPath"
    }

    foreach ($group in $groups) {
        $import = $xml.CreateElement('Import', $nsUri)
        $null = $import.SetAttribute('Project', $relativeProps)
        $null = $import.SetAttribute('Condition', "exists('$relativeProps')")
        $group.AppendChild($import) | Out-Null
    }

    $xml.Save($ProjectPath)
    Write-Log "Inserted props import into $(Split-Path -Leaf $ProjectPath)" Green
}

function Prepend-MacroPreservingDefault {
    param(
        [Parameter()][string]$ExistingValue,
        [Parameter(Mandatory = $true)][string]$Macro,
        [Parameter(Mandatory = $true)][string]$DefaultMacro
    )

    $existing = [string]$ExistingValue
    if ([string]::IsNullOrWhiteSpace($existing)) {
        return "$Macro;$DefaultMacro"
    }

    $parts = @()
    foreach ($piece in ($existing -split ';')) {
        $trimmed = $piece.Trim()
        if (-not [string]::IsNullOrWhiteSpace($trimmed)) {
            $parts += $trimmed
        }
    }

    if ($parts -notcontains $DefaultMacro) {
        $parts += $DefaultMacro
    }

    $parts = @($parts | Where-Object { $_ -ne $Macro })
    return (@($Macro) + $parts) -join ';'
}

function Remove-MacroPreservingDefault {
    param(
        [Parameter()][string]$ExistingValue,
        [Parameter(Mandatory = $true)][string]$Macro,
        [Parameter(Mandatory = $true)][string]$DefaultMacro
    )

    $existing = [string]$ExistingValue
    $parts = @()
    foreach ($piece in ($existing -split ';')) {
        $trimmed = $piece.Trim()
        if (-not [string]::IsNullOrWhiteSpace($trimmed) -and $trimmed -ne $Macro) {
            $parts += $trimmed
        }
    }

    if ($parts.Count -eq 0) {
        return $DefaultMacro
    }

    if ($parts -notcontains $DefaultMacro) {
        $parts += $DefaultMacro
    }

    return ($parts | Select-Object -Unique) -join ';'
}

function Get-OrCreateChildElement {
    param(
        [Parameter(Mandatory = $true)]$Xml,
        [Parameter(Mandatory = $true)]$Parent,
        [Parameter(Mandatory = $true)][string]$NsUri,
        [Parameter(Mandatory = $true)][string]$ChildName,
        [Parameter(Mandatory = $true)]$NamespaceManager
    )

    $node = $Parent.SelectSingleNode("msb:$ChildName", $NamespaceManager)
    if (-not $node) {
        $node = $Xml.CreateElement($ChildName, $NsUri)
        $Parent.AppendChild($node) | Out-Null
    }
    return $node
}

function Update-ProjectDependencyPaths {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][ValidateSet('SVG','FFMpeg')][string]$Kind
    )

    $ctx = Get-MsbuildXmlDocument -ProjectPath $ProjectPath
    $xml = $ctx.Xml
    $nsmgr = $ctx.Ns
    $nsUri = $ctx.NsUri

    if ($Kind -eq 'SVG') {
        $includeMacro = '$(MysticThumbsResvgInclude)'
        $libMacro = '$(MysticThumbsResvgLib)'
    }
    else {
        $includeMacro = '$(MysticThumbsFFmpegInclude)'
        $libMacro = '$(MysticThumbsFFmpegLib)'
    }

    # Clean up earlier script versions that wrote directly to VC++ Directories IncludePath/LibraryPath.
    $propertyGroups = $xml.SelectNodes("//msb:PropertyGroup[@Condition]", $nsmgr)
    foreach ($group in $propertyGroups) {
        $includePathNode = $group.SelectSingleNode("msb:IncludePath", $nsmgr)
        if ($includePathNode) {
            $includePathNode.InnerText = Remove-MacroPreservingDefault -ExistingValue $includePathNode.InnerText -Macro $includeMacro -DefaultMacro '$(IncludePath)'
        }

        $libraryPathNode = $group.SelectSingleNode("msb:LibraryPath", $nsmgr)
        if ($libraryPathNode) {
            $libraryPathNode.InnerText = Remove-MacroPreservingDefault -ExistingValue $libraryPathNode.InnerText -Macro $libMacro -DefaultMacro '$(LibraryPath)'
        }
    }

    # Preferred integration point: C/C++ and Link additional directories.
    $itemGroups = $xml.SelectNodes("//msb:ItemDefinitionGroup", $nsmgr)
    if (-not $itemGroups -or $itemGroups.Count -eq 0) {
        throw "Could not find any ItemDefinitionGroup elements in $ProjectPath"
    }

    foreach ($itemGroup in $itemGroups) {
        $cl = Get-OrCreateChildElement -Xml $xml -Parent $itemGroup -NsUri $nsUri -ChildName 'ClCompile' -NamespaceManager $nsmgr
        $addInc = Get-OrCreateChildElement -Xml $xml -Parent $cl -NsUri $nsUri -ChildName 'AdditionalIncludeDirectories' -NamespaceManager $nsmgr
        $addInc.InnerText = Prepend-MacroPreservingDefault -ExistingValue $addInc.InnerText -Macro $includeMacro -DefaultMacro '%(AdditionalIncludeDirectories)'

        $link = Get-OrCreateChildElement -Xml $xml -Parent $itemGroup -NsUri $nsUri -ChildName 'Link' -NamespaceManager $nsmgr
        $addLib = Get-OrCreateChildElement -Xml $xml -Parent $link -NsUri $nsUri -ChildName 'AdditionalLibraryDirectories' -NamespaceManager $nsmgr
        $addLib.InnerText = Prepend-MacroPreservingDefault -ExistingValue $addLib.InnerText -Macro $libMacro -DefaultMacro '%(AdditionalLibraryDirectories)'
    }

    $xml.Save($ProjectPath)
    Write-Log "Updated AdditionalIncludeDirectories / AdditionalLibraryDirectories in $(Split-Path -Leaf $ProjectPath)" Green
}

function Update-VcxprojDependencyConfig {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][ValidateSet('SVG','FFMpeg')][string]$Kind
    )

    Update-ProjectDependencyPaths -ProjectPath $ProjectPath -Kind $Kind
}

function Update-GitIgnoreHint {
    $gitIgnorePath = Join-Path $RepoRoot '.gitignore'
    $recommended = @('External/', 'MysticThumbs.ExternalDeps.props')

    if (-not (Test-Path -LiteralPath $gitIgnorePath)) {
        Write-Log '.gitignore not found. Skipping automatic update.' Yellow
        return
    }

    $existing = @(Get-Content -LiteralPath $gitIgnorePath -ErrorAction SilentlyContinue)
    $toAdd = @()
    foreach ($line in $recommended) {
        if ($existing -notcontains $line) { $toAdd += $line }
    }

    if ($toAdd.Count -gt 0) {
        Add-Content -LiteralPath $gitIgnorePath -Value "`r`n# Local external toolchains`r`n$($toAdd -join "`r`n")"
        Write-Log 'Updated .gitignore with External/ and MysticThumbs.ExternalDeps.props' Green
    }
    else {
        Write-Log '.gitignore already contains External/ and props entries.' DarkCyan
    }
}

function Get-ProjectCandidates {
    param([Parameter(Mandatory = $true)][string]$LeafName)

    $candidates = Get-ChildItem -Path $RepoRoot -Filter $LeafName -File -Recurse -ErrorAction SilentlyContinue |
        Sort-Object FullName |
        Select-Object -ExpandProperty FullName

    return @($candidates)
}

function Show-StartupDialog {
    $form = New-Object System.Windows.Forms.Form
    $form.Text = "Voith's CODE - Setup resvg and FFmpeg for MysticThumbs"
    $form.Size = New-Object System.Drawing.Size(760, 360)
    $form.StartPosition = 'CenterScreen'
    $form.FormBorderStyle = 'FixedDialog'
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false

    $intro = New-Object System.Windows.Forms.Label
    $intro.Location = New-Object System.Drawing.Point(20, 18)
    $intro.Size = New-Object System.Drawing.Size(700, 56)
    $intro.Text = "This setup uses a repo-local External directory and generates MysticThumbs.ExternalDeps.props automatically."
    $form.Controls.Add($intro)

    $fields = @(
        @{ Y = 90;  Label = 'Repo root:';     Value = $RepoRoot },
        @{ Y = 130; Label = 'External root:'; Value = $ExternalRoot },
        @{ Y = 170; Label = 'resvg source:';  Value = $ResvgRepoRoot },
        @{ Y = 210; Label = 'vcpkg root:';    Value = $VcpkgRoot },
        @{ Y = 250; Label = 'Props file:';    Value = $PropsPath }
    )

    foreach ($f in $fields) {
        $lbl = New-Object System.Windows.Forms.Label
        $lbl.Location = New-Object System.Drawing.Point(20, [int]$f['Y'])
        $lbl.Size = New-Object System.Drawing.Size(120, 20)
        $lbl.Text = [string]$f['Label']
        $form.Controls.Add($lbl)

        $tb = New-Object System.Windows.Forms.TextBox
        $tb.Location = New-Object System.Drawing.Point(145, ([int]$f['Y'] - 3))
        $tb.Size = New-Object System.Drawing.Size(575, 23)
        $tb.ReadOnly = $true
        $tb.Text = [string]$f['Value']
        $form.Controls.Add($tb)
    }

    $startButton = New-Object System.Windows.Forms.Button
    $startButton.Location = New-Object System.Drawing.Point(470, 290)
    $startButton.Size = New-Object System.Drawing.Size(110, 30)
    $startButton.Text = 'Start Setup'
    $startButton.DialogResult = [System.Windows.Forms.DialogResult]::OK
    $form.Controls.Add($startButton)

    $cancelButton = New-Object System.Windows.Forms.Button
    $cancelButton.Location = New-Object System.Drawing.Point(600, 290)
    $cancelButton.Size = New-Object System.Drawing.Size(110, 30)
    $cancelButton.Text = 'Cancel'
    $cancelButton.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
    $form.Controls.Add($cancelButton)

    $form.AcceptButton = $startButton
    $form.CancelButton = $cancelButton

    return $form.ShowDialog()
}


function Show-CompletionDialog {
    param([Parameter(Mandatory = $true)][string]$Text)

    $font = New-Object System.Drawing.Font('Consolas', 10)
    $lines = $Text -split "`r?`n"
    $longestLine = ($lines | Measure-Object -Maximum Length).Maximum
    if (-not $longestLine) { $longestLine = 80 }

    $charWidth = [int][Math]::Ceiling($font.Size * 0.95)
    $targetClientWidth = [Math]::Min(1400, [Math]::Max(860, ($longestLine * $charWidth) + 48))
    $targetClientHeight = 520

    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'Setup Complete'
    $form.ClientSize = New-Object System.Drawing.Size($targetClientWidth, $targetClientHeight)
    $form.StartPosition = 'CenterScreen'
    $form.FormBorderStyle = 'Sizable'
    $form.MaximizeBox = $true
    $form.MinimizeBox = $false
    $form.MinimumSize = New-Object System.Drawing.Size(900, 600)

    $textBox = New-Object System.Windows.Forms.TextBox
    $textBox.Multiline = $true
    $textBox.ReadOnly = $true
    $textBox.ScrollBars = 'Both'
    $textBox.WordWrap = $false
    $textBox.Location = New-Object System.Drawing.Point(16, 16)
    $textBox.Size = New-Object System.Drawing.Size(($form.ClientSize.Width - 32), ($form.ClientSize.Height - 86))
    $textBox.Anchor = 'Top,Bottom,Left,Right'
    $textBox.Font = $font
    $textBox.Text = $Text
    $form.Controls.Add($textBox)

    $okButton = New-Object System.Windows.Forms.Button
    $okButton.Text = 'OK'
    $okButton.Size = New-Object System.Drawing.Size(100, 30)
    $okButton.Location = New-Object System.Drawing.Point(($form.ClientSize.Width - 116), ($form.ClientSize.Height - 46))
    $okButton.Anchor = 'Bottom,Right'
    $okButton.Add_Click({ $form.Close() })
    $form.Controls.Add($okButton)

    $form.AcceptButton = $okButton

    [void]$form.ShowDialog()
}

function Run-Setup {
    Write-Log 'Starting refactored setup...' Cyan

    if (-not (Test-IsAdmin)) {
        Write-Log 'WARNING: Script is not running elevated.' Yellow
        $confirm = [System.Windows.Forms.MessageBox]::Show(
            'Running as Administrator is still recommended. Continue anyway?',
            'Administrator Recommended',
            [System.Windows.Forms.MessageBoxButtons]::YesNo,
            [System.Windows.Forms.MessageBoxIcon]::Warning
        )
        if ($confirm -ne [System.Windows.Forms.DialogResult]::Yes) {
            throw 'Setup cancelled because the script was not elevated.'
        }
    }

    Assert-Command -Name 'git' -HelpText 'Install Git for Windows first.'

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $msg = 'cl.exe was not found. Please launch install_resvg_and_ffmpeg.cmd from a VS2022 Developer Command Prompt so the MSVC toolchain is loaded.'
        Write-Log $msg Yellow
        $confirm = [System.Windows.Forms.MessageBox]::Show(
            $msg + "`n`nContinue anyway?",
            'Visual Studio Developer Prompt Recommended',
            [System.Windows.Forms.MessageBoxButtons]::YesNo,
            [System.Windows.Forms.MessageBoxIcon]::Warning
        )
        if ($confirm -ne [System.Windows.Forms.DialogResult]::Yes) {
            throw 'Setup cancelled because MSVC tools were not available in PATH.'
        }
    }

    Ensure-Directory -Path $ExternalRoot
    Ensure-Directory -Path $ExternalSrcRoot

    Ensure-RustToolchain
    Ensure-GitCloneOrPull -RepoUrl 'https://github.com/linebender/resvg.git' -DestinationPath $ResvgRepoRoot
    Build-Resvg -Target 'x86_64-pc-windows-msvc' -Prefix (Join-Path $ResvgInstallRoot 'x64')
    Build-Resvg -Target 'i686-pc-windows-msvc' -Prefix (Join-Path $ResvgInstallRoot 'x86')

    Ensure-Vcpkg
    Write-ExternalDepsProps

    $svgProj = Get-ExpectedProjectPath -RelativePath 'SVGPlugin\SVGPluginMysticThumbs.vcxproj'
    $ffmpegProj = Get-ExpectedProjectPath -RelativePath 'FFMpegPlugin\FFMpegPluginMysticThumbs.vcxproj'

    if ($svgProj) {
        Ensure-PropsImport -ProjectPath $svgProj
        Update-VcxprojDependencyConfig -ProjectPath $svgProj -Kind 'SVG'
    }
    else {
        Write-Log 'SVGPlugin\SVGPluginMysticThumbs.vcxproj was not found. Skipping SVG project patch.' Yellow
    }

    if ($ffmpegProj) {
        Ensure-PropsImport -ProjectPath $ffmpegProj
        Update-VcxprojDependencyConfig -ProjectPath $ffmpegProj -Kind 'FFMpeg'
    }
    else {
        Write-Log 'FFMpegPlugin\FFMpegPluginMysticThumbs.vcxproj was not found. Skipping FFmpeg project patch.' Yellow
    }

    Update-GitIgnoreHint

    $summaryLines = @(
        'Setup completed successfully.',
        '',
        'Created / updated:',
        "- $ExternalRoot",
        "- $PropsPath",
        "- $LogPath",
        '',
        'Installed toolchains:',
        '- resvg source:',
        "  $ResvgRepoRoot",
        '- resvg x64:',
        "  $(Join-Path $ResvgInstallRoot 'x64')",
        '- resvg x86:',
        "  $(Join-Path $ResvgInstallRoot 'x86')",
        '- vcpkg:',
        "  $VcpkgRoot",
        '- FFmpeg x64:',
        "  $(Join-Path $VcpkgRoot 'installed\x64-windows')",
        '- FFmpeg x86:',
        "  $(Join-Path $VcpkgRoot 'installed\x86-windows')",
        '',
        'Patched project files:',
        '- SVGPlugin\SVGPluginMysticThumbs.vcxproj',
        '- FFMpegPlugin\FFMpegPluginMysticThumbs.vcxproj',
        '',
        'Project wiring:',
        '- Each project now imports MysticThumbs.ExternalDeps.props',
        '- SVG uses $(MysticThumbsResvgInclude) and $(MysticThumbsResvgLib)',
        '- FFMpeg uses $(MysticThumbsFFmpegInclude) and $(MysticThumbsFFmpegLib)',
        '',
        'Reminder:',
        '- The FFmpeg runtime DLLs still need to sit alongside the built',
        '  .mtp plugin DLLs when you test or deploy the FFmpeg plugin.'
    )

    $summary = $summaryLines -join "`r`n"

    Write-Log $summary Green
    Show-CompletionDialog -Text $summary
}

try {
    $dialogResult = Show-StartupDialog
    if ($dialogResult -eq [System.Windows.Forms.DialogResult]::OK) {
        Run-Setup
    }
    else {
        Write-Log 'Setup cancelled by user.' Yellow
    }
}
catch {
    $message = $_.Exception.Message
    Write-Log "ERROR: $message" Red
    [System.Windows.Forms.MessageBox]::Show(
        $message + "`n`nSee log:`n$LogPath",
        'Setup Failed',
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error
    ) | Out-Null
    exit 1
}

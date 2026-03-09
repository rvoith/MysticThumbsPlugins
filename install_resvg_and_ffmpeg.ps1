
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$ScriptPath = $MyInvocation.MyCommand.Path
$RepoRoot   = Split-Path -Parent $ScriptPath
$LogPath    = Join-Path $RepoRoot "install_resvg_and_ffmpeg.log"

$ExternalRoot   = Join-Path $RepoRoot "External"
$ExternalSrc    = Join-Path $ExternalRoot "src"
$ResvgSrc       = Join-Path $ExternalSrc "resvg"
$ResvgInstall   = Join-Path $ExternalRoot "resvg\resvg-capi"
$ResvgX64       = Join-Path $ResvgInstall "x64"
$ResvgX86       = Join-Path $ResvgInstall "x86"
$VcpkgRoot      = Join-Path $ExternalRoot "vcpkg"
$PropsPath      = Join-Path $RepoRoot "MysticThumbs.ExternalDeps.props"

$FFmpegDllPatterns = @(
    "avutil-*.dll",
    "swscale-*.dll",
    "swresample-*.dll",
    "avcodec-*.dll",
    "avformat-*.dll",
    "avfilter-*.dll",
    "avdevice-*.dll",
    "dav1d.dll"
)

$SummaryLines = New-Object System.Collections.Generic.List[string]

function Write-Log {
    param(
        [Parameter(Mandatory = $true)][string]$Message,
        [ConsoleColor]$Color = [ConsoleColor]::Gray
    )
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $Message
    Write-Host $line -ForegroundColor $Color
    Add-Content -LiteralPath $LogPath -Value $line -Encoding UTF8
}

function Add-Summary {
    param([string]$Line)
    [void]$SummaryLines.Add($Line)
}

function Ensure-Directory {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        [void](New-Item -ItemType Directory -Path $Path -Force)
    }
}

function Join-Args {
    param([string[]]$Arguments)
    if (-not $Arguments -or $Arguments.Count -eq 0) { return "" }

    $parts = foreach ($arg in $Arguments) {
        if ($null -eq $arg) { continue }
        if ($arg -match '[\s"]') {
            '"' + ($arg -replace '"', '\"') + '"'
        } else {
            $arg
        }
    }
    return ($parts -join ' ')
}

function Invoke-LoggedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = $RepoRoot,
        [hashtable]$Environment = @{},
        [switch]$IgnoreExitCode
    )

    $argString = Join-Args $Arguments
    Write-Log ("RUN: {0} {1}" -f $FilePath, $argString) Cyan

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.Arguments = $argString
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    foreach ($key in $Environment.Keys) {
        $psi.EnvironmentVariables[$key] = [string]$Environment[$key]
    }

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi

    [void]$proc.Start()
    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()

    if ($stdout) {
        foreach ($line in ($stdout -split "`r?`n")) {
            if ($line -ne "") { Write-Log $line DarkGray }
        }
    }
    if ($stderr) {
        foreach ($line in ($stderr -split "`r?`n")) {
            if ($line -ne "") { Write-Log $line Yellow }
        }
    }

    if (-not $IgnoreExitCode -and $proc.ExitCode -ne 0) {
        throw "Command failed with exit code $($proc.ExitCode): $FilePath $argString"
    }

    return $proc.ExitCode
}

function Assert-Command {
    param(
        [Parameter(Mandatory = $true)][string]$CommandName,
        [Parameter(Mandatory = $true)][string]$FriendlyName
    )
    if (-not (Get-Command $CommandName -ErrorAction SilentlyContinue)) {
        throw "$FriendlyName was not found in PATH. Please run this from a Developer Command Prompt for VS2022 and ensure $FriendlyName is installed."
    }
}

function Install-RustIfMissing {
    if (Get-Command cargo -ErrorAction SilentlyContinue) { return }

    Write-Log "cargo was not found. Installing Rust toolchain..." Yellow
    $rustupExe = Join-Path $env:TEMP "rustup-init.exe"

    try {
        Invoke-WebRequest -Uri "https://win.rustup.rs/x86_64" -OutFile $rustupExe -UseBasicParsing
    }
    catch {
        Invoke-WebRequest -Uri "https://win.rustup.rs/" -OutFile $rustupExe -UseBasicParsing
    }

    Invoke-LoggedProcess -FilePath $rustupExe -Arguments @("-y")

    $cargoBin = Join-Path $env:USERPROFILE ".cargo\bin"
    if (Test-Path -LiteralPath $cargoBin) {
        $env:PATH = $env:PATH + ";" + $cargoBin
    }
}

function Get-RelativePathCompat {
    param(
        [Parameter(Mandatory = $true)][string]$BasePath,
        [Parameter(Mandatory = $true)][string]$TargetPath
    )

    $baseFull = [System.IO.Path]::GetFullPath($BasePath)
    $targetFull = [System.IO.Path]::GetFullPath($TargetPath)

    if (-not $baseFull.EndsWith('\')) { $baseFull += '\' }

    $baseUri = New-Object System.Uri($baseFull)
    $targetUri = New-Object System.Uri($targetFull)
    $relativeUri = $baseUri.MakeRelativeUri($targetUri)
    return [System.Uri]::UnescapeDataString($relativeUri.ToString()).Replace('/', '\')
}

function Get-ExpectedProjectPath {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$ProjectSubdir,
        [Parameter(Mandatory = $true)][string]$ProjectFileName
    )
    return Join-Path (Join-Path $RepoRoot $ProjectSubdir) $ProjectFileName
}

function Select-NodesLocalName {
    param(
        [Parameter(Mandatory = $true)]$Node,
        [Parameter(Mandatory = $true)][string]$XPath
    )
    return $Node.SelectNodes($XPath)
}

function Select-SingleNodeLocalName {
    param(
        [Parameter(Mandatory = $true)]$Node,
        [Parameter(Mandatory = $true)][string]$XPath
    )
    return $Node.SelectSingleNode($XPath)
}

function Normalize-SemicolonList {
    param([string]$Value)
    if ([string]::IsNullOrWhiteSpace($Value)) { return "" }

    $items = New-Object System.Collections.Generic.List[string]
    foreach ($part in ($Value -split ';')) {
        $trimmed = $part.Trim()
        if ($trimmed -ne "" -and -not $items.Contains($trimmed)) {
            [void]$items.Add($trimmed)
        }
    }
    return ($items -join ';')
}

function Prepend-TokenToList {
    param(
        [string]$Existing,
        [Parameter(Mandatory = $true)][string]$Token,
        [Parameter(Mandatory = $true)][string]$FallbackTail
    )

    $items = New-Object System.Collections.Generic.List[string]
    foreach ($part in (($Existing -split ';') + @($FallbackTail))) {
        $trimmed = $part.Trim()
        if ($trimmed -ne "") {
            [void]$items.Add($trimmed)
        }
    }

    $filtered = New-Object System.Collections.Generic.List[string]
    [void]$filtered.Add($Token)
    foreach ($item in $items) {
        if ($item -ne $Token -and -not $filtered.Contains($item)) {
            [void]$filtered.Add($item)
        }
    }
    return Normalize-SemicolonList ($filtered -join ';')
}

function Remove-TokenFromList {
    param(
        [string]$Existing,
        [Parameter(Mandatory = $true)][string]$Token
    )
    if ([string]::IsNullOrWhiteSpace($Existing)) { return $Existing }

    $items = New-Object System.Collections.Generic.List[string]
    foreach ($part in ($Existing -split ';')) {
        $trimmed = $part.Trim()
        if ($trimmed -ne "" -and $trimmed -ne $Token) {
            [void]$items.Add($trimmed)
        }
    }
    if ($items.Count -eq 0) { return "" }
    return Normalize-SemicolonList ($items -join ';')
}

function Show-IntroDialog {
    $form = New-Object System.Windows.Forms.Form
    $form.Text = "Install resvg and FFmpeg"
    $form.StartPosition = "CenterScreen"
    $form.Width = 760
    $form.Height = 320
    $form.FormBorderStyle = 'FixedDialog'
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false

    $label = New-Object System.Windows.Forms.Label
    $label.AutoSize = $false
    $label.Left = 20
    $label.Top = 20
    $label.Width = 700
    $label.Height = 180
    $label.Text = @"
This script will install the required resvg and FFmpeg toolchains locally inside this repository.

It will create:
  External\resvg\resvg-capi\x64
  External\resvg\resvg-capi\x86
  External\vcpkg

It will also:
  - generate MysticThumbs.ExternalDeps.props in the repo root
  - patch SVGPlugin and FFMpegPlugin project files to use the local dependencies
  - copy the FFmpeg runtime DLLs into the MysticThumbs plugin Debug/Release folders under %APPDATA%

Run this from a Developer Command Prompt for VS2022 as administrator.
"@

    $startBtn = New-Object System.Windows.Forms.Button
    $startBtn.Text = "Start Setup"
    $startBtn.Width = 120
    $startBtn.Height = 32
    $startBtn.Left = 460
    $startBtn.Top = 230
    $startBtn.DialogResult = [System.Windows.Forms.DialogResult]::OK

    $cancelBtn = New-Object System.Windows.Forms.Button
    $cancelBtn.Text = "Cancel"
    $cancelBtn.Width = 120
    $cancelBtn.Height = 32
    $cancelBtn.Left = 595
    $cancelBtn.Top = 230
    $cancelBtn.DialogResult = [System.Windows.Forms.DialogResult]::Cancel

    $form.AcceptButton = $startBtn
    $form.CancelButton = $cancelBtn

    $form.Controls.Add($label)
    $form.Controls.Add($startBtn)
    $form.Controls.Add($cancelBtn)

    return ($form.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK)
}

function Show-CompletionDialog {
    param([Parameter(Mandatory = $true)][string]$Text)

    $lines = $Text -split "`r?`n"
    $bmp = New-Object System.Drawing.Bitmap(1, 1)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $font = New-Object System.Drawing.Font("Consolas", 10)

    $maxWidth = 0
    foreach ($line in $lines) {
        $size = $g.MeasureString($line, $font)
        $lineWidth = [int][Math]::Ceiling($size.Width)
        if ($lineWidth -gt $maxWidth) { $maxWidth = $lineWidth }
    }
    $g.Dispose()
    $bmp.Dispose()

    $form = New-Object System.Windows.Forms.Form
    $form.Text = "Setup Complete"
    $form.StartPosition = "CenterScreen"
    $form.MinimumSize = New-Object System.Drawing.Size(760, 460)
    $form.Width = [Math]::Min([Math]::Max($maxWidth + 80, 820), 1350)
    $form.Height = 600

    $panel = New-Object System.Windows.Forms.Panel
    $panel.Dock = "Bottom"
    $panel.Height = 52
    $panel.Padding = New-Object System.Windows.Forms.Padding(0, 8, 12, 8)

    $textBox = New-Object System.Windows.Forms.TextBox
    $textBox.Multiline = $true
    $textBox.ReadOnly = $true
    $textBox.ScrollBars = "Both"
    $textBox.WordWrap = $false
    $textBox.Dock = "Fill"
    $textBox.Font = $font
    $textBox.Text = $Text

    $okButton = New-Object System.Windows.Forms.Button
    $okButton.Text = "OK"
    $okButton.Width = 100
    $okButton.Height = 30
    $okButton.Top = 10
    $okButton.Left = $panel.Width - 112
    $okButton.Anchor = "Bottom,Right"
    $okButton.DialogResult = [System.Windows.Forms.DialogResult]::OK
    $okButton.Add_Click({ $form.Close() })

    $form.AcceptButton = $okButton
    $form.CancelButton = $okButton

    $panel.Controls.Add($okButton)

    # Important: add the bottom panel first, then the fill control,
    # otherwise the fill textbox can occupy the entire client area and visually hide the panel.
    $form.Controls.Add($panel)
    $form.Controls.Add($textBox)

    [void]$form.ShowDialog()
}

function Ensure-ResvgBuilt {
    Ensure-Directory $ExternalRoot
    Ensure-Directory $ExternalSrc
    Ensure-Directory $ResvgInstall

    Install-RustIfMissing
    Assert-Command "git" "Git"
    Assert-Command "rustup" "rustup"
    Assert-Command "cargo" "cargo"

    Write-Log "Configuring Rust targets..." Green
    Invoke-LoggedProcess -FilePath "rustup" -Arguments @("default", "stable")
    Invoke-LoggedProcess -FilePath "rustup" -Arguments @("target", "add", "x86_64-pc-windows-msvc")
    Invoke-LoggedProcess -FilePath "rustup" -Arguments @("target", "add", "i686-pc-windows-msvc")

    if (-not (Get-Command cargo-cbuild -ErrorAction SilentlyContinue)) {
        Write-Log "Installing cargo-c..." Green
        Invoke-LoggedProcess -FilePath "cargo" -Arguments @("install", "cargo-c", "--locked")
    }

    if (-not (Test-Path -LiteralPath $ResvgSrc)) {
        Write-Log "Cloning resvg..." Green
        Invoke-LoggedProcess -FilePath "git" -Arguments @("clone", "https://github.com/linebender/resvg.git", $ResvgSrc) -WorkingDirectory $ExternalSrc
    }
    else {
        Write-Log "resvg source already exists: $ResvgSrc" DarkCyan
    }

    Write-Log "Building resvg C API x64..." Green
    Ensure-Directory $ResvgX64
    Invoke-LoggedProcess -FilePath "cargo" -WorkingDirectory $ResvgSrc -Arguments @(
        "cinstall", "--release", "--locked",
        "--manifest-path", "crates/c-api/Cargo.toml",
        "--target", "x86_64-pc-windows-msvc",
        "--prefix", $ResvgX64
    ) -Environment @{ RUSTFLAGS = "-C target-feature=+crt-static" }

    Write-Log "Building resvg C API x86..." Green
    Ensure-Directory $ResvgX86
    Invoke-LoggedProcess -FilePath "cargo" -WorkingDirectory $ResvgSrc -Arguments @(
        "cinstall", "--release", "--locked",
        "--manifest-path", "crates/c-api/Cargo.toml",
        "--target", "i686-pc-windows-msvc",
        "--prefix", $ResvgX86
    ) -Environment @{ RUSTFLAGS = "-C target-feature=+crt-static" }

    Add-Summary "Installed toolchains:"
    Add-Summary " - resvg source:"
    Add-Summary "   $ResvgSrc"
    Add-Summary " - resvg x64:"
    Add-Summary "   $ResvgX64"
    Add-Summary " - resvg x86:"
    Add-Summary "   $ResvgX86"
}

function Ensure-VcpkgAndFFmpeg {
    Ensure-Directory $ExternalRoot
    Assert-Command "git" "Git"

    if (-not (Test-Path -LiteralPath $VcpkgRoot)) {
        Write-Log "Cloning vcpkg..." Green
        Invoke-LoggedProcess -FilePath "git" -Arguments @("clone", "https://github.com/microsoft/vcpkg.git", $VcpkgRoot) -WorkingDirectory $ExternalRoot
    }
    else {
        Write-Log "vcpkg already exists: $VcpkgRoot" DarkCyan
    }

    $bootstrap = Join-Path $VcpkgRoot "bootstrap-vcpkg.bat"
    if (-not (Test-Path -LiteralPath $bootstrap)) {
        throw "bootstrap-vcpkg.bat not found at $bootstrap"
    }

    Write-Log "Bootstrapping vcpkg..." Green
    Invoke-LoggedProcess -FilePath $bootstrap -WorkingDirectory $VcpkgRoot

    $vcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
    if (-not (Test-Path -LiteralPath $vcpkgExe)) {
        throw "vcpkg.exe not found after bootstrap."
    }

    Write-Log "Installing FFmpeg x64..." Green
    Invoke-LoggedProcess -FilePath $vcpkgExe -WorkingDirectory $VcpkgRoot -Arguments @("install", "ffmpeg[dav1d]:x64-windows")

    Write-Log "Installing FFmpeg x86..." Green
    Invoke-LoggedProcess -FilePath $vcpkgExe -WorkingDirectory $VcpkgRoot -Arguments @("install", "ffmpeg[dav1d]:x86-windows", "--allow-unsupported")

    Add-Summary " - vcpkg:"
    Add-Summary "   $VcpkgRoot"
    Add-Summary " - FFmpeg x64:"
    Add-Summary "   $(Join-Path $VcpkgRoot 'installed\x64-windows')"
    Add-Summary " - FFmpeg x86:"
    Add-Summary "   $(Join-Path $VcpkgRoot 'installed\x86-windows')"
}

function Write-ExternalDepsProps {
    $content = @'
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Condition="'$(Platform)'=='x64'">
    <MysticThumbsResvgInclude>$(MSBuildThisFileDirectory)External\resvg\resvg-capi\x64\include</MysticThumbsResvgInclude>
    <MysticThumbsResvgLib>$(MSBuildThisFileDirectory)External\resvg\resvg-capi\x64\lib</MysticThumbsResvgLib>
    <MysticThumbsFFmpegInclude>$(MSBuildThisFileDirectory)External\vcpkg\installed\x64-windows\include</MysticThumbsFFmpegInclude>
    <MysticThumbsFFmpegLib>$(MSBuildThisFileDirectory)External\vcpkg\installed\x64-windows\lib</MysticThumbsFFmpegLib>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Platform)'=='Win32'">
    <MysticThumbsResvgInclude>$(MSBuildThisFileDirectory)External\resvg\resvg-capi\x86\include</MysticThumbsResvgInclude>
    <MysticThumbsResvgLib>$(MSBuildThisFileDirectory)External\resvg\resvg-capi\x86\lib</MysticThumbsResvgLib>
    <MysticThumbsFFmpegInclude>$(MSBuildThisFileDirectory)External\vcpkg\installed\x86-windows\include</MysticThumbsFFmpegInclude>
    <MysticThumbsFFmpegLib>$(MSBuildThisFileDirectory)External\vcpkg\installed\x86-windows\lib</MysticThumbsFFmpegLib>
  </PropertyGroup>
</Project>
'@
    Set-Content -LiteralPath $PropsPath -Value $content -Encoding UTF8
    Write-Log "Wrote props file: $PropsPath" Green
}

function Ensure-PropsImport {
    param([Parameter(Mandatory = $true)][string]$ProjectPath)

    if (-not (Test-Path -LiteralPath $ProjectPath)) {
        Write-Log "Project not found: $ProjectPath" Yellow
        return
    }

    [xml]$xml = Get-Content -LiteralPath $ProjectPath -Raw -Encoding UTF8
    $relativeProps = Get-RelativePathCompat -BasePath (Split-Path -Parent $ProjectPath) -TargetPath $PropsPath

    $groups = @(Select-NodesLocalName -Node $xml -XPath "/*[local-name()='Project']/*[local-name()='ImportGroup' and @Label='PropertySheets']")
    if ($groups.Count -eq 0) {
        Write-Log "No PropertySheets import group found in $(Split-Path -Leaf $ProjectPath)" Yellow
        return
    }

    $changed = $false
    foreach ($group in $groups) {
        $already = $false
        foreach ($child in $group.ChildNodes) {
            if ($child.LocalName -eq "Import" -and $child.Project -eq $relativeProps) {
                $already = $true
                break
            }
        }
        if (-not $already) {
            $newImport = $xml.CreateElement("Import", $xml.DocumentElement.NamespaceURI)
            [void]$newImport.SetAttribute("Project", $relativeProps)
            [void]$newImport.SetAttribute("Condition", "exists('$relativeProps')")
            [void]$group.AppendChild($newImport)
            $changed = $true
        }
    }

    if ($changed) {
        $xml.Save($ProjectPath)
        Write-Log "Inserted props import into $(Split-Path -Leaf $ProjectPath)" Green
    }
    else {
        Write-Log "Props import already present in $(Split-Path -Leaf $ProjectPath)" DarkCyan
    }
}
function Update-VcxprojDependencyConfig {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$IncludeMacro,
        [Parameter(Mandatory = $true)][string]$LibMacro
    )

    if (-not (Test-Path -LiteralPath $ProjectPath)) {
        Write-Log "Project not found: $ProjectPath" Yellow
        return
    }

    [xml]$xml = Get-Content -LiteralPath $ProjectPath -Raw -Encoding UTF8
    $changed = $false

    $idgs = @(Select-NodesLocalName -Node $xml -XPath "/*[local-name()='Project']/*[local-name()='ItemDefinitionGroup']")
    foreach ($idg in $idgs) {
        $cl = Select-SingleNodeLocalName -Node $idg -XPath "./*[local-name()='ClCompile']"
        if (-not $cl) {
            $cl = $xml.CreateElement("ClCompile", $xml.DocumentElement.NamespaceURI)
            [void]$idg.AppendChild($cl)
            $changed = $true
        }

        $addInc = Select-SingleNodeLocalName -Node $cl -XPath "./*[local-name()='AdditionalIncludeDirectories']"
        if (-not $addInc) {
            $addInc = $xml.CreateElement("AdditionalIncludeDirectories", $xml.DocumentElement.NamespaceURI)
            [void]$cl.AppendChild($addInc)
            $changed = $true
        }

        $newInc = Prepend-TokenToList -Existing $addInc.InnerText -Token $IncludeMacro -FallbackTail "%(AdditionalIncludeDirectories)"
        if ($addInc.InnerText -ne $newInc) {
            $addInc.InnerText = $newInc
            $changed = $true
        }

        $link = Select-SingleNodeLocalName -Node $idg -XPath "./*[local-name()='Link']"
        if (-not $link) {
            $link = $xml.CreateElement("Link", $xml.DocumentElement.NamespaceURI)
            [void]$idg.AppendChild($link)
            $changed = $true
        }

        $addLib = Select-SingleNodeLocalName -Node $link -XPath "./*[local-name()='AdditionalLibraryDirectories']"
        if (-not $addLib) {
            $addLib = $xml.CreateElement("AdditionalLibraryDirectories", $xml.DocumentElement.NamespaceURI)
            [void]$link.AppendChild($addLib)
            $changed = $true
        }

        $newLib = Prepend-TokenToList -Existing $addLib.InnerText -Token $LibMacro -FallbackTail "%(AdditionalLibraryDirectories)"
        if ($addLib.InnerText -ne $newLib) {
            $addLib.InnerText = $newLib
            $changed = $true
        }
    }

    $pgs = @(Select-NodesLocalName -Node $xml -XPath "/*[local-name()='Project']/*[local-name()='PropertyGroup']")
    foreach ($pg in $pgs) {
        $includePath = Select-SingleNodeLocalName -Node $pg -XPath "./*[local-name()='IncludePath']"
        if ($includePath) {
            $cleaned = Remove-TokenFromList -Existing $includePath.InnerText -Token $IncludeMacro
            if ([string]::IsNullOrWhiteSpace($cleaned)) { $cleaned = '$(IncludePath)' }
            if ($includePath.InnerText -ne $cleaned) {
                $includePath.InnerText = $cleaned
                $changed = $true
            }
        }

        $libraryPath = Select-SingleNodeLocalName -Node $pg -XPath "./*[local-name()='LibraryPath']"
        if ($libraryPath) {
            $cleaned = Remove-TokenFromList -Existing $libraryPath.InnerText -Token $LibMacro
            if ([string]::IsNullOrWhiteSpace($cleaned)) { $cleaned = '$(LibraryPath)' }
            if ($libraryPath.InnerText -ne $cleaned) {
                $libraryPath.InnerText = $cleaned
                $changed = $true
            }
        }
    }

    if ($changed) {
        $xml.Save($ProjectPath)
        Write-Log "Patched dependency settings in $(Split-Path -Leaf $ProjectPath)" Green
    }
    else {
        Write-Log "Dependency settings already correct in $(Split-Path -Leaf $ProjectPath)" DarkCyan
    }
}
function Get-FFMpegConfigurations {
    param([Parameter(Mandatory = $true)][string]$ProjectPath)

    $result = @{
        "x64"   = @()
        "Win32" = @()
    }

    if (-not (Test-Path -LiteralPath $ProjectPath)) {
        $result["x64"] = @("Debug", "Release")
        $result["Win32"] = @("Debug", "Release")
        return $result
    }

    [xml]$xml = Get-Content -LiteralPath $ProjectPath -Raw -Encoding UTF8
    $nodes = @(Select-NodesLocalName -Node $xml -XPath "/*[local-name()='Project']/*[local-name()='ItemGroup']/*[local-name()='ProjectConfiguration']")

    foreach ($node in $nodes) {
        $include = [string]$node.Include
        if ($include -match '^([^|]+)\|([^|]+)$') {
            $cfg  = $matches[1]
            $plat = $matches[2]
            if (($plat -eq 'x64' -or $plat -eq 'Win32') -and ($result[$plat] -notcontains $cfg)) {
                $result[$plat] = @($result[$plat]) + @($cfg)
            }
        }
    }

    if (@($result["x64"]).Count -eq 0)   { $result["x64"] = @("Debug", "Release") }
    if (@($result["Win32"]).Count -eq 0) { $result["Win32"] = @("Debug", "Release") }

    return $result
}
function Get-MatchingDlls {
    param(
        [Parameter(Mandatory = $true)][string]$BinDir,
        [Parameter(Mandatory = $true)][string[]]$Patterns
    )

    $matches = New-Object System.Collections.Generic.List[System.IO.FileInfo]
    foreach ($pattern in $Patterns) {
        $files = @(Get-ChildItem -LiteralPath $BinDir -Filter $pattern -File -ErrorAction SilentlyContinue | Sort-Object Name)
        if ($files.Count -eq 0) {
            Write-Log "No DLL matched pattern '$pattern' in $BinDir" Yellow
            continue
        }

        $chosen = $files[0]
        [void]$matches.Add($chosen)
    }

    return @($matches)
}

function Deploy-FFmpegRuntimeDlls {
    param([Parameter(Mandatory = $true)][string]$FFMpegProjectPath)

    $cfgs = Get-FFMpegConfigurations -ProjectPath $FFMpegProjectPath
    $appPluginsRoot = Join-Path $env:APPDATA "MysticThumbs\Plugins"
    $x64Bin = Join-Path $VcpkgRoot "installed\x64-windows\bin"
    $x86Bin = Join-Path $VcpkgRoot "installed\x86-windows\bin"

    if (-not (Test-Path -LiteralPath $x64Bin)) { throw "FFmpeg x64 bin directory not found: $x64Bin" }
    if (-not (Test-Path -LiteralPath $x86Bin)) { throw "FFmpeg x86 bin directory not found: $x86Bin" }

    $x64Dlls = Get-MatchingDlls -BinDir $x64Bin -Patterns $FFmpegDllPatterns
    $x86Dlls = Get-MatchingDlls -BinDir $x86Bin -Patterns $FFmpegDllPatterns

    $copiedTotal = 0
    $existingTotal = 0

    foreach ($cfg in @($cfgs["x64"])) {
        $targetDir = Join-Path $appPluginsRoot ("64\" + $cfg)
        Ensure-Directory $targetDir

        foreach ($dll in $x64Dlls) {
            $dst = Join-Path $targetDir $dll.Name
            if (Test-Path -LiteralPath $dst) {
                $existingTotal++
            } else {
                Copy-Item -LiteralPath $dll.FullName -Destination $dst
                $copiedTotal++
            }
        }
        Add-Summary (" - x64 runtime DLLs target: {0}" -f $targetDir)
    }

    foreach ($cfg in @($cfgs["Win32"])) {
        $targetDir = Join-Path $appPluginsRoot ("32\" + $cfg)
        Ensure-Directory $targetDir

        foreach ($dll in $x86Dlls) {
            $dst = Join-Path $targetDir $dll.Name
            if (Test-Path -LiteralPath $dst) {
                $existingTotal++
            } else {
                Copy-Item -LiteralPath $dll.FullName -Destination $dst
                $copiedTotal++
            }
        }
        Add-Summary (" - x86 runtime DLLs target: {0}" -f $targetDir)
    }

    Add-Summary " - FFmpeg DLL patterns:"
    foreach ($pattern in $FFmpegDllPatterns) {
        Add-Summary ("   {0}" -f $pattern)
    }

    Write-Log ("FFmpeg runtime DLL deployment complete. Copied={0}, AlreadyPresent={1}" -f $copiedTotal, $existingTotal) Green
}

try {
    "" | Set-Content -LiteralPath $LogPath -Encoding UTF8

    if (-not (Show-IntroDialog)) {
        Write-Log "Setup cancelled by user." Yellow
        exit 0
    }

    Write-Log "Starting setup..." Green

    Add-Summary "Setup completed successfully."
    Add-Summary ""
    Add-Summary "Created / updated:"
    Add-Summary " - $ExternalRoot"
    Add-Summary " - $PropsPath"
    Add-Summary ""

    Ensure-ResvgBuilt
    Ensure-VcpkgAndFFmpeg
    Write-ExternalDepsProps

    $svgProjectPath    = Get-ExpectedProjectPath -RepoRoot $RepoRoot -ProjectSubdir "SVGPlugin"    -ProjectFileName "SVGPluginMysticThumbs.vcxproj"
    $ffmpegProjectPath = Get-ExpectedProjectPath -RepoRoot $RepoRoot -ProjectSubdir "FFMpegPlugin" -ProjectFileName "FFMpegPluginMysticThumbs.vcxproj"

    Ensure-PropsImport -ProjectPath $svgProjectPath
    Ensure-PropsImport -ProjectPath $ffmpegProjectPath

    Update-VcxprojDependencyConfig -ProjectPath $svgProjectPath    -IncludeMacro '$(MysticThumbsResvgInclude)'   -LibMacro '$(MysticThumbsResvgLib)'
    Update-VcxprojDependencyConfig -ProjectPath $ffmpegProjectPath -IncludeMacro '$(MysticThumbsFFmpegInclude)' -LibMacro '$(MysticThumbsFFmpegLib)'

    Add-Summary ""
    Add-Summary "Project integration:"
    Add-Summary " - SVG project:"
    Add-Summary "   $svgProjectPath"
    Add-Summary " - FFMpeg project:"
    Add-Summary "   $ffmpegProjectPath"

    Add-Summary ""
    Add-Summary "FFmpeg runtime DLL deployment:"
    Deploy-FFmpegRuntimeDlls -FFMpegProjectPath $ffmpegProjectPath

    Add-Summary ""
    Add-Summary "The script generated MysticThumbs.ExternalDeps.props"
    Add-Summary "and patched the SVG and FFMpeg project files to use it."
    Add-Summary ""
    Add-Summary "Log file:"
    Add-Summary " - $LogPath"

    Show-CompletionDialog ($SummaryLines -join "`r`n")
    exit 0
}
catch {
    $msg = $_.Exception.Message
    Write-Log "ERROR: $msg" Red
    [System.Windows.Forms.MessageBox]::Show(
        "ERROR: $msg`r`n`r`nSee log:`r`n$LogPath",
        "Setup Failed",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error
    ) | Out-Null
    exit 1
}

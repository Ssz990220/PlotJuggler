<#
.SYNOPSIS
  Build the PlotJuggler 4 Windows installer (Qt Installer Framework, offline).

.DESCRIPTION
  Stages the already-built pj_app.exe plus its runtime dependencies into a
  scratch packages tree, then runs binarycreator to produce one self-contained
  .exe. The source packages directory (installer/packages/) is NOT mutated by
  this script — everything is staged under $env:TEMP so a build never dirties
  the working tree.

    1. locate the built pj_app.exe under -BuildDir
    2. stage it under <stage>/packages/io.plotjuggler.application/data/PlotJuggler4.exe
    3. windeployqt the exe AND every plugin DLL — matches PJ3 (a plugin can
       pull Qt modules the main exe does not)
    4. copy the FFmpeg + CPython + other Conan-shared runtime DLLs from the
       Conan cache that produced the build (constrained to package bin dirs)
    5. render config.xml / package.xml from templates into the stage tree
    6. binarycreator --offline-only -> PlotJuggler-<Version>-Windows-x64.exe

  This does NOT build the app. Run it AFTER a Windows build, e.g.
    conan install . --output-folder=build --build=missing \
        -s build_type=RelWithDebInfo -s compiler.cppstd=20 \
        -o "cpython/*:shared=True" -s "cpython/*:build_type=Release"
    cmake --preset ... && cmake --build build --config RelWithDebInfo

.PARAMETER BuildDir
  The PJ4 build directory (default: build). Searched for plotjuggler4.exe /
  pj_app.exe under RelWithDebInfo/ (preferred) then Release/.

.PARAMETER QtDir
  Qt kit dir that owns windeployqt.exe, e.g. C:\Qt\6.11.1\msvc2022_64.
  If empty, windeployqt is taken from PATH.

.PARAMETER IfwDir
  Directory containing binarycreator.exe. If empty, it is taken from PATH or
  auto-detected under C:\Qt\Tools\QtInstallerFramework\*\bin.

.PARAMETER PluginsDir
  Optional dir whose *.dll plugins are bundled next to the app (default: none).

.PARAMETER ConanHome
  Conan 2 cache root (default: %USERPROFILE%\.conan2). Source of the FFmpeg /
  CPython / other shared runtime DLLs that the build linked against.

.PARAMETER Version
  Installer version string, also used in the output file name. Default reads
  PJ_APP_VERSION from repo-root versions.env when present, otherwise falls
  back to the value hard-coded here.

.PARAMETER OutDir
  Where to write the installer .exe (default: current directory).

.PARAMETER StageDir
  Scratch directory for the staged package tree (default: $env:TEMP\pj4-installer-stage).
  Wiped at the start of every run.

.NOTES
  Host requirements:
    - Qt 6.11.1 msvc2022_64 (windeployqt.exe)
    - Qt Installer Framework tools (binarycreator.exe) -- install via the Qt
      Maintenance Tool, or:  aqt install-tool windows desktop tools_ifw
    - A populated Conan cache from the PJ4 build (FFmpeg + CPython + Draco DLLs)
#>
param(
  [string]$BuildDir   = "build",
  [string]$QtDir      = "",
  [string]$IfwDir     = "",
  [string]$PluginsDir = "",
  [string]$ConanHome  = "$env:USERPROFILE\.conan2",
  [string]$Version    = "",
  [string]$OutDir     = ".",
  [string]$StageDir   = ""
)

$ErrorActionPreference = "Stop"
function Info($m)  { Write-Host "[installer] $m" -ForegroundColor Cyan }
function Warn($m)  { Write-Host "[installer] WARNING: $m" -ForegroundColor Yellow }
function Die($m)   { Write-Host "[installer] ERROR: $m" -ForegroundColor Red; exit 1 }

$installerRoot = Split-Path -Parent $MyInvocation.MyCommand.Path      # ...\installer
$repoRoot      = Split-Path -Parent $installerRoot
$pkgSrc        = Join-Path $installerRoot "packages\io.plotjuggler.application"
$configXmlSrc  = Join-Path $installerRoot "config.xml"
$packageXmlSrc = Join-Path $pkgSrc        "meta\package.xml"

# --- default version from versions.env -------------------------------------
if (-not $Version) {
  $versionsEnv = Join-Path $repoRoot "versions.env"
  if (Test-Path $versionsEnv) {
    $line = Get-Content $versionsEnv | Where-Object { $_ -match '^\s*PJ_APP_VERSION\s*=\s*(.+?)\s*$' } | Select-Object -First 1
    if ($line -and $Matches[1]) { $Version = $Matches[1] }
  }
}
if (-not $Version) { $Version = "3.999.1" }  # last-resort fallback
Info "target version: $Version"

# --- stage directory (scratch, wiped on every run) ------------------------
if (-not $StageDir) { $StageDir = Join-Path $env:TEMP "pj4-installer-stage" }
if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }
$stagePackages = Join-Path $StageDir "packages"
$stagePkgRoot  = Join-Path $stagePackages "io.plotjuggler.application"
$stageMeta     = Join-Path $stagePkgRoot "meta"
$stageData     = Join-Path $stagePkgRoot "data"
New-Item -ItemType Directory -Force -Path $stageMeta,$stageData | Out-Null
Info "stage: $StageDir"

# --- resolve tools ---------------------------------------------------------
function Resolve-Exe([string]$name, [string]$hintDir) {
  if ($hintDir -and (Test-Path (Join-Path $hintDir $name))) { return (Join-Path $hintDir $name) }
  $cmd = Get-Command $name -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  return $null
}

$qtBinDir = if ($QtDir) { Join-Path $QtDir "bin" } else { "" }
$windeployqt = Resolve-Exe "windeployqt.exe" $qtBinDir
if (-not $windeployqt) { Die "windeployqt.exe not found. Pass -QtDir C:\Qt\6.11.1\msvc2022_64 or add it to PATH." }
Info "windeployqt : $windeployqt"

$binarycreator = Resolve-Exe "binarycreator.exe" $IfwDir
if (-not $binarycreator) {
  $cand = Get-ChildItem "C:\Qt\Tools\QtInstallerFramework" -Recurse -Filter "binarycreator.exe" -ErrorAction SilentlyContinue |
          Sort-Object FullName -Descending | Select-Object -First 1
  if ($cand) { $binarycreator = $cand.FullName }
}
if (-not $binarycreator) { Die "binarycreator.exe not found. Install the Qt Installer Framework tools (aqt install-tool windows desktop tools_ifw) and/or pass -IfwDir." }
Info "binarycreator: $binarycreator"

# --- locate the built app --------------------------------------------------
# Prefer RelWithDebInfo (CI default), then Release, then any config. The CMake
# target 'pj_app' has OUTPUT_NAME plotjuggler4 on main, but older builds still
# ship as pj_app.exe — accept either.
function Find-AppExe {
  foreach ($cfg in @("RelWithDebInfo","Release","*")) {
    foreach ($name in @("plotjuggler4.exe","pj_app.exe")) {
      $glob = if ($cfg -eq "*") { $name } else { "$cfg\$name" }
      $hit = Get-ChildItem $BuildDir -Recurse -Filter $name -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -like "*\$glob" -or $cfg -eq "*" } |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
      if ($hit) { return $hit }
    }
  }
  return $null
}
$appExe = Find-AppExe
if (-not $appExe) { Die "plotjuggler4.exe / pj_app.exe not found under '$BuildDir'. Build PJ4 first (cmake --build build --config RelWithDebInfo)." }
Info "app binary  : $($appExe.FullName)"

# --- stage the data payload ------------------------------------------------
$stagedExe = Join-Path $stageData "PlotJuggler4.exe"
Copy-Item $appExe.FullName $stagedExe
Info "staged PlotJuggler4.exe"

# optional bundled plugins next to the app
$pluginDlls = @()
if ($PluginsDir -and (Test-Path $PluginsDir)) {
  $pluginDlls = Get-ChildItem $PluginsDir -Filter "*.dll" -ErrorAction SilentlyContinue
  foreach ($d in $pluginDlls) { Copy-Item $d.FullName $stageData }
  Info "staged $($pluginDlls.Count) plugin DLL(s) from $PluginsDir"
}

# --- Qt deployment (Qt DLLs + Qt plugins + MSVC runtime) -------------------
# windeployqt has to be run over the exe AND every plugin DLL: a plugin can
# pull Qt modules (Qt Charts, Qt SVG, Qt QuickWidgets …) the main exe does
# not, and without walking each plugin those transitive Qt DLLs never land in
# the bundle — the classic "window doesn't render fully" symptom.
Info "running windeployqt on exe + plugins ..."
& $windeployqt --release --no-translations --compiler-runtime $stagedExe
if ($LASTEXITCODE -ne 0) { Die "windeployqt failed on the exe (exit $LASTEXITCODE)." }
foreach ($d in (Get-ChildItem $stageData -Filter "*.dll")) {
  & $windeployqt --release --no-translations $d.FullName
  if ($LASTEXITCODE -ne 0) { Die "windeployqt failed on $($d.Name) (exit $LASTEXITCODE)." }
}

# --- copy non-Qt runtime DLLs from the Conan cache -------------------------
# windeployqt only knows about Qt; Conan-shared deps are copied by hand.
#
# Constrain the search to Conan 2 package "bin" folders (~/.conan2/p/<hash>/p/bin)
# — WITHOUT that constraint the recursive scan also picks up copies in build
# trees (~/.conan2/p/b/<hash>/build/...), the "newest by mtime" may be one of
# those, and the sibling DLL copy pulls unrelated intermediates.
function Copy-ConanRuntime([string]$probeDll, [string]$label, [switch]$Required) {
  $hit = Get-ChildItem $ConanHome -Recurse -Filter $probeDll -ErrorAction SilentlyContinue |
         Where-Object { $_.FullName -match '\\p\\[^\\]+\\p\\bin\\' } |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not $hit) {
    if ($Required) { Die "$label runtime not found in Conan cache ($ConanHome\p\*\p\bin\$probeDll). Was the build done with this cache?" }
    Warn "$label runtime ($probeDll) not found in Conan cache -- skipping (bundle it manually if the app needs it)."
    return
  }
  $srcDir = $hit.Directory.FullName
  $copied = 0
  foreach ($d in (Get-ChildItem $srcDir -Filter "*.dll")) { Copy-Item $d.FullName $stageData -Force; $copied++ }
  Info "staged $copied $label DLL(s) from $srcDir"
}

Copy-ConanRuntime "avcodec*.dll" "FFmpeg"     -Required
Copy-ConanRuntime "python3*.dll" "CPython"    -Required
Copy-ConanRuntime "dav1d*.dll"   "libdav1d"           # AV1 decode via FFmpeg
Copy-ConanRuntime "draco*.dll"   "Draco"               # compressed pointcloud decode
Copy-ConanRuntime "zstd*.dll"    "zstd"
Copy-ConanRuntime "lz4*.dll"     "LZ4"
Copy-ConanRuntime "mcap*.dll"    "mcap"

# CPython needs its stdlib next to the DLL for the embedded interpreter to
# initialise. Copy Lib/, DLLs/ and any python3XX._pth from the same Conan
# package root as the python3XX.dll we just staged.
function Copy-CPythonStdlib {
  $pyDll = Get-ChildItem $ConanHome -Recurse -Filter "python3*.dll" -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -match '\\p\\[^\\]+\\p\\bin\\' } |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not $pyDll) { Warn "CPython DLL missing -- stdlib bundling skipped."; return }
  $pyPkgRoot = Split-Path -Parent (Split-Path -Parent $pyDll.FullName)   # ...\p\<hash>\p
  $libSrc  = Join-Path $pyPkgRoot "Lib"
  $dllsSrc = Join-Path $pyPkgRoot "DLLs"
  if (Test-Path $libSrc)  {
    Copy-Item $libSrc  $stageData -Recurse -Force
    Info "staged CPython Lib/ from $libSrc"
  } else { Warn "CPython Lib/ not found under $pyPkgRoot -- pj_scripting may fail to init the interpreter." }
  if (Test-Path $dllsSrc) {
    Copy-Item $dllsSrc $stageData -Recurse -Force
    Info "staged CPython DLLs/ from $dllsSrc"
  }
  $pth = Get-ChildItem $pyDll.Directory.FullName -Filter "python3*._pth" -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($pth) { Copy-Item $pth.FullName $stageData -Force; Info "staged $($pth.Name)" }
}
Copy-CPythonStdlib

# --- render config.xml + package.xml from source into the stage tree -------
# Templates carry a __PJ_VERSION__ token; the source XMLs stay clean so a
# build never dirties the working tree (the classic "why does my checkout
# look modified" trap).
function Render-Template([string]$src, [string]$dst) {
  (Get-Content -Raw $src) `
    -replace '__PJ_VERSION__', $Version `
    -replace '__PJ_RELEASE_DATE__', (Get-Date -Format 'yyyy-MM-dd') |
    Set-Content $dst
}
Render-Template $configXmlSrc  (Join-Path $StageDir "config.xml")
Render-Template $packageXmlSrc (Join-Path $stageMeta "package.xml")

# Non-templated meta files (script, UI, licenses) go verbatim.
foreach ($f in Get-ChildItem (Join-Path $pkgSrc "meta") -File | Where-Object { $_.Name -ne "package.xml" }) {
  Copy-Item $f.FullName $stageMeta -Force
}

# --- build the installer ---------------------------------------------------
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
$outExe = Join-Path (Resolve-Path $OutDir) "PlotJuggler-$Version-Windows-x64.exe"
Info "running binarycreator -> $outExe"
& $binarycreator --offline-only -c (Join-Path $StageDir "config.xml") -p $stagePackages $outExe
if ($LASTEXITCODE -ne 0) { Die "binarycreator failed (exit $LASTEXITCODE)." }

Info "DONE: $outExe"

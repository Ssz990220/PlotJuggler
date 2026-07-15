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
    2. stage it under <stage>/packages/io.plotjuggler.application/data/bin/PlotJuggler4.exe
    3. build the ported plugins from source (unless -SkipPlugins) with conan+cmake
       natively — no Git Bash — then bundle their *_plugin.dll into
       data/lib/plotjuggler/plugins (the AppImage flow)
    4. windeployqt the exe AND every bundled plugin DLL — matches PJ3 (a plugin can
       pull Qt modules the main exe does not)
    5. copy the FFmpeg + CPython + other Conan-shared runtime DLLs from the
       Conan cache that produced the build (constrained to package bin dirs)
    6. render config.xml / package.xml from templates into the stage tree
    7. binarycreator --offline-only -> <YYYY.MM.DD>.PlotJuggler-<Version>-Windows-x64.<main-commit>.exe

  This does NOT build the app (only, optionally, the plugins). Run it AFTER a
  Windows build of the app, e.g.
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

.PARAMETER SkipPlugins
  Skip building/bundling plugins for a fast core-only installer. By default the
  script BUILDS the ported plugins from source (the AppImage flow) and bundles the
  resulting *_plugin.dll — reproducible on any machine, no pre-built dir needed.

.PARAMETER PortedPluginsDir
  Location of the ported-plugins tree (each plugin is a subdir with a conanfile.py).
  Defaults to <repo>\plotjuggler_sdk\pj_ported_plugins.

.PARAMETER PluginList
  Plugins to build (default: the curated set). Each name must be a plugin subdir of
  -PortedPluginsDir. A plugin that fails to build is skipped (not fatal); the rest ship.

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
    - Qt 6.11.1 msvc2022_64 (windeployqt.exe) -- auto-found in .qt when present
    - Qt Installer Framework tools (binarycreator.exe) -- install via the Qt
      Maintenance Tool, or:  aqt install-tool --outputdir .qt windows desktop tools_ifw
    - Conan + CMake on PATH -- builds the ported plugins natively (unless
      -SkipPlugins); no Git Bash needed. The MSVC x64 toolchain (cl.exe) is
      auto-activated via vswhere/vcvarsall if not already in a VS dev prompt, and
      Ninja is auto-located (PATH / Conan cache / Python Scripts; pip install ninja
      if absent). Needs Visual Studio with the C++ x64 workload.
    - A populated Conan cache from the PJ4 build (FFmpeg + CPython DLLs)
#>
param(
  [string]$BuildDir   = "build",
  [string]$QtDir      = "",
  [string]$IfwDir     = "",
  [switch]$SkipPlugins,
  [string]$PortedPluginsDir = "",
  [string[]]$PluginList = @(
    "data_load_mcap", "data_load_csv", "data_load_parquet", "data_load_ulog",
    "data_stream_dummy", "data_stream_foxglove_bridge", "data_stream_pj_bridge",
    "parser_ros", "parser_protobuf", "parser_json",
    "toolbox_mosaico", "toolbox_quaternion", "toolbox_transform_editor"
  ),
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
# Prefix layout the installed app expects: <prefix>/bin holds the exe + all its
# runtime DLLs; plugins live in <prefix>/lib/plotjuggler/plugins, which the app
# resolves relative to the exe (bin -> ../lib/plotjuggler/plugins) and scans
# recursively. config.xml's TargetDir is <prefix>; $stageData maps to it.
$stageBin      = Join-Path $stageData "bin"
$stagePlugins  = Join-Path $stageData "lib\plotjuggler\plugins"
New-Item -ItemType Directory -Force -Path $stageMeta,$stageBin,$stagePlugins | Out-Null
Info "stage: $StageDir"

# --- resolve tools ---------------------------------------------------------
function Resolve-Exe([string]$name, [string]$hintDir) {
  if ($hintDir -and (Test-Path (Join-Path $hintDir $name))) { return (Join-Path $hintDir $name) }
  $cmd = Get-Command $name -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  return $null
}

# Default -QtDir to the aqt layout inside the repo (.qt) when present, so a machine
# that installed Qt via install_qt6.sh / aqt needs no -QtDir.
if (-not $QtDir) {
  $qtGuess = Join-Path $repoRoot ".qt\6.11.1\msvc2022_64"
  if (Test-Path (Join-Path $qtGuess "bin\windeployqt.exe")) { $QtDir = $qtGuess; Info "auto-detected Qt: $QtDir" }
}
$qtBinDir = if ($QtDir) { Join-Path $QtDir "bin" } else { "" }
$windeployqt = Resolve-Exe "windeployqt.exe" $qtBinDir
if (-not $windeployqt) { Die "windeployqt.exe not found. Pass -QtDir <kit> (e.g. .qt\6.11.1\msvc2022_64 or C:\Qt\6.11.1\msvc2022_64) or add it to PATH." }
Info "windeployqt : $windeployqt"

$binarycreator = Resolve-Exe "binarycreator.exe" $IfwDir
if (-not $binarycreator) {
  # Search the usual IFW-tool locations. aqt lays them under <aqt-base>\Tools\
  # QtInstallerFramework, where <aqt-base> is two levels above the kit dir
  # (e.g. .qt\6.11.1\msvc2022_64 -> .qt); the Qt online installer uses C:\Qt\Tools.
  $ifwRoots = @()
  if ($QtDir) {
    $aqtBase = Split-Path -Parent (Split-Path -Parent $QtDir)
    if ($aqtBase) { $ifwRoots += (Join-Path $aqtBase "Tools\QtInstallerFramework") }
  }
  $ifwRoots += "C:\Qt\Tools\QtInstallerFramework"
  foreach ($root in $ifwRoots) {
    if (-not (Test-Path $root)) { continue }
    $cand = Get-ChildItem $root -Recurse -Filter "binarycreator.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
    if ($cand) { $binarycreator = $cand.FullName; break }
  }
}
if (-not $binarycreator) { Die "binarycreator.exe not found. Install the Qt Installer Framework tools (aqt install-tool --outputdir .qt windows desktop tools_ifw) and/or pass -IfwDir." }
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
$stagedExe = Join-Path $stageBin "PlotJuggler4.exe"
Copy-Item $appExe.FullName $stagedExe
Info "staged bin\PlotJuggler4.exe"

# Ensure the MSVC x64 toolchain (cl.exe) is on PATH. The Conan profile builds with
# compiler=msvc and the Ninja generator invokes cl directly, so cl must resolve. If
# we are not already in a VS dev prompt, locate VS via vswhere and import
# `vcvarsall.bat x64` into this process — the same thing CI's msvc-dev-cmd does — so
# the recipe works from a plain PowerShell too, not only an x64 Native Tools prompt.
function Enter-MsvcEnv {
  if (Get-Command cl -ErrorAction SilentlyContinue) { Info "cl.exe already on PATH"; return }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) {
    Die "cl.exe not on PATH and vswhere.exe not found. Run from an 'x64 Native Tools Command Prompt for VS', or install the Visual Studio C++ tools."
  }
  $vsPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                        -property installationPath 2>$null | Select-Object -First 1)
  if (-not $vsPath) { Die "No Visual Studio with the C++ x64 toolset found via vswhere. Install the 'Desktop development with C++' workload, or run from an x64 Native Tools prompt." }
  $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
  if (-not (Test-Path $vcvars)) { Die "vcvarsall.bat not found under $vsPath." }
  Info "activating MSVC x64 via $vcvars"
  # Run vcvarsall then dump `set`; import each var into this process. Silence the
  # banner and gate `set` on vcvars succeeding.
  $prevEAP = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  cmd /c "`"$vcvars`" x64 >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
  }
  $ErrorActionPreference = $prevEAP
  $cl = Get-Command cl -ErrorAction SilentlyContinue
  if (-not $cl) { Die "Activated MSVC but cl.exe still not found -- verify the VS C++ x64 toolset is installed." }
  Info "cl.exe : $($cl.Source)"
}

# Build one ported plugin natively — the same three steps pj_ported_plugins/build.sh
# runs per plugin: conan install the plugin recipe, cmake-configure the tree selecting
# just this plugin (-DPJ_BUILD_PLUGIN), then build. cpython is forced shared so
# python3XX.dll exists to bundle; -s:b compiler.cppstd=20 satisfies the build-context
# protobuf/protoc (needs C++17). Returns $true on success; external output streams to
# the host (Out-Host) so it is NOT captured into the return value.
function Build-PortedPlugin([string]$portedDir, [string]$plugin, [string]$ninjaExe) {
  $src = Join-Path $portedDir $plugin
  if (-not (Test-Path (Join-Path $src "conanfile.py"))) {
    Warn "plugin ${plugin}: no conanfile.py under $src -- skipping"; return $false
  }
  $bdir  = Join-Path $portedDir "build\$plugin"
  $cbdir = Join-Path $bdir "Release"
  # Native build tools write progress to stderr and a failed plugin exits non-zero;
  # under the script's $ErrorActionPreference='Stop' a stderr line merged with 2>&1
  # raises a terminating NativeCommandError (aborting even a successful build). Relax
  # to 'Continue' (function-local) and DON'T merge stderr — let it flow to the console
  # natively — then gate purely on $LASTEXITCODE. stdout still goes to Out-Host so it
  # is not captured into the boolean return value.
  $ErrorActionPreference = 'Continue'
  & conan install $src "--output-folder=$bdir" --build=missing `
      -s build_type=Release -s compiler.cppstd=20 -s:b compiler.cppstd=20 `
      -c tools.cmake.cmaketoolchain:generator=Ninja `
      -o "cpython/*:shared=True" | Out-Host
  if ($LASTEXITCODE -ne 0) { return $false }
  # -DCMAKE_MAKE_PROGRAM: the Conan profile forces the Ninja generator, but CMake
  # still has to FIND ninja. Pass its resolved path explicitly so the build does not
  # depend on ninja being on PATH (it often isn't in a bare MSVC/PowerShell prompt).
  & cmake -S $portedDir -B $cbdir -G Ninja `
      "-DCMAKE_MAKE_PROGRAM=$ninjaExe" `
      "-DCMAKE_TOOLCHAIN_FILE=$bdir\conan_toolchain.cmake" `
      "-DCMAKE_PREFIX_PATH=$bdir" -DCMAKE_BUILD_TYPE=Release "-DPJ_BUILD_PLUGIN=$plugin" | Out-Host
  if ($LASTEXITCODE -ne 0) { return $false }
  & cmake --build $cbdir --config Release --parallel | Out-Host
  return ($LASTEXITCODE -eq 0)
}

# --- build the ported plugins from source, then bundle their *_plugin.dll ---------
# AppImage flow: compile, then package. Building from source (rather than depending
# on a pre-existing plugins dir) is what makes the recipe reproducible on any
# machine. Each plugin is built NATIVELY here (no Git Bash) with the same three
# steps pj_ported_plugins/build.sh runs per plugin — conan/cmake/ninja are the
# same cross-platform CLIs the app build already needs. Skip with -SkipPlugins.
$pluginDlls = @()
if ($SkipPlugins) {
  Info "skipping plugins (-SkipPlugins) -> core-only installer"
} else {
  if (-not $PortedPluginsDir) { $PortedPluginsDir = Join-Path $repoRoot "plotjuggler_sdk\pj_ported_plugins" }
  if (-not (Test-Path (Join-Path $PortedPluginsDir "CMakeLists.txt"))) {
    Die "Ported-plugins tree not found at $PortedPluginsDir. Pass -PortedPluginsDir or -SkipPlugins."
  }
  Enter-MsvcEnv   # cl.exe on PATH (activates vcvars x64 if not already in a dev prompt)
  foreach ($tool in @("conan", "cmake")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
      Die "$tool not found on PATH. Run from an MSVC x64 dev prompt with Conan/CMake available, or pass -SkipPlugins."
    }
  }
  # The Conan profile forces the Ninja generator, so CMake needs ninja.exe. It is
  # frequently NOT on a bare MSVC/PowerShell PATH (pip installs it into Python's
  # Scripts dir). Resolve it once, here, and hand the path to each build; Die with a
  # clear instruction rather than letting CMake emit "unable to find Ninja" x13.
  $ninjaExe = $null
  $ninjaCmd = Get-Command ninja -ErrorAction SilentlyContinue
  if ($ninjaCmd) { $ninjaExe = $ninjaCmd.Source }
  if (-not $ninjaExe) {
    $cacheNinja = Get-ChildItem $ConanHome -Recurse -Filter "ninja.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cacheNinja) { $ninjaExe = $cacheNinja.FullName }
  }
  if (-not $ninjaExe) {
    $pyCmd = Get-Command python -ErrorAction SilentlyContinue
    if ($pyCmd) {
      $scriptsDir = (& $pyCmd.Source -c "import sysconfig; print(sysconfig.get_path('scripts'))" 2>$null | Select-Object -First 1)
      if ($scriptsDir -and (Test-Path (Join-Path $scriptsDir "ninja.exe"))) { $ninjaExe = Join-Path $scriptsDir "ninja.exe" }
    }
  }
  if (-not $ninjaExe) { Die "ninja not found on PATH, in the Conan cache, or in Python's Scripts dir. Install it (pip install ninja) or add it to PATH -- the Conan profile forces the Ninja generator." }
  Info "ninja       : $ninjaExe"

  Info "building $($PluginList.Count) ported plugin(s) in $PortedPluginsDir ..."
  $okPlugins = @(); $failPlugins = @()
  foreach ($p in $PluginList) {
    Info "──── building plugin: $p ────"
    if (Build-PortedPlugin $PortedPluginsDir $p $ninjaExe) { $okPlugins += $p } else { $failPlugins += $p; Warn "plugin build FAILED: $p" }
  }
  Info ("plugin builds OK ({0}): {1}" -f $okPlugins.Count, ($okPlugins -join ' '))
  if ($failPlugins.Count) { Warn ("plugin builds FAILED ({0}): {1}" -f $failPlugins.Count, ($failPlugins -join ' ')) }

  # Collect the built plugins (per-plugin output: build/<plugin>/<cfg>/...). Match
  # ONLY *_plugin.dll (the PJ naming convention) so the recursive scan never drags in
  # dependency DLLs (cpython, etc.). De-dupe by name so a plugin built under several
  # configs (Debug / RelWithDebInfo) is staged once.
  $pluginsBuildTree = Join-Path $PortedPluginsDir "build"
  $pluginDlls = Get-ChildItem $pluginsBuildTree -Recurse -Filter "*_plugin.dll" -ErrorAction SilentlyContinue |
                Sort-Object Name -Unique
  foreach ($d in $pluginDlls) { Copy-Item $d.FullName $stagePlugins -Force }
  Info "staged $($pluginDlls.Count) plugin DLL(s) -> lib\plotjuggler\plugins"
  if ($pluginDlls.Count -eq 0) { Warn "no *_plugin.dll found under $pluginsBuildTree — the installer will have no plugins." }
}

# --- Qt deployment (Qt DLLs + Qt plugins + MSVC runtime) -------------------
# windeployqt has to be run over the exe AND every plugin DLL: a plugin can
# pull Qt modules (Qt Charts, Qt SVG, Qt QuickWidgets …) the main exe does
# not, and without walking each plugin those transitive Qt DLLs never land in
# the bundle — the classic "window doesn't render fully" symptom.
Info "running windeployqt on exe + plugins ..."
& $windeployqt --release --no-translations --compiler-runtime $stagedExe
if ($LASTEXITCODE -ne 0) { Die "windeployqt failed on the exe (exit $LASTEXITCODE)." }
# Only the bundled plugin DLLs — NOT every DLL in data\, which by now also holds the
# non-Qt files windeployqt itself dropped (D3Dcompiler_47.dll, opengl32sw.dll, …);
# running windeployqt over those makes it exit 1. Empty when -SkipPlugins.
foreach ($d in $pluginDlls) {
  $stagedPlugin = Join-Path $stagePlugins $d.Name
  # --dir $stageBin: the plugin lives under lib\..., but its extra Qt module DLLs
  # must land in bin\ next to the exe (the dir Windows searches at load time), not
  # next to the plugin — otherwise a plugin-only Qt module never resolves.
  & $windeployqt --dir $stageBin --release --no-translations $stagedPlugin
  # Warn, don't Die: a recursively-collected DLL that is not a Qt-linked plugin
  # makes windeployqt exit 1, which must not abort the whole bundle.
  if ($LASTEXITCODE -ne 0) { Warn "windeployqt on $($d.Name) returned $LASTEXITCODE -- harmless if it is not a Qt-linked plugin." }
}

# --- copy non-Qt runtime DLLs from the Conan cache -------------------------
# windeployqt only knows about Qt; Conan-shared deps are copied by hand.
#
# Constrain the search to Conan 2 package "bin" folders: both the downloaded
# layout (~/.conan2/p/<hash>/p/bin) and the built-from-source layout
# (~/.conan2/p/b/<hash>/p/bin, note the extra "b" segment). WITHOUT that
# constraint the recursive scan also picks up copies under a package's build
# tree (~/.conan2/p/b/<hash>/build/...), the "newest by mtime" may be one of
# those, and the sibling DLL copy pulls unrelated intermediates.
function Copy-ConanRuntime([string]$probeDll, [string]$label, [switch]$Required) {
  $hit = Get-ChildItem $ConanHome -Recurse -Filter $probeDll -ErrorAction SilentlyContinue |
         Where-Object { $_.FullName -match '\\p\\(?:b\\)?[^\\]+\\p\\bin\\' } |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not $hit) {
    if ($Required) { Die "$label runtime not found in Conan cache ($ConanHome\p\[b\]*\p\bin\$probeDll). Was the build done with this cache?" }
    Warn "$label runtime ($probeDll) not found in Conan cache -- skipping (bundle it manually if the app needs it)."
    return
  }
  $srcDir = $hit.Directory.FullName
  $copied = 0
  foreach ($d in (Get-ChildItem $srcDir -Filter "*.dll")) { Copy-Item $d.FullName $stageBin -Force; $copied++ }
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
           Where-Object { $_.FullName -match '\\p\\(?:b\\)?[^\\]+\\p\\bin\\' } |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not $pyDll) { Warn "CPython DLL missing -- stdlib bundling skipped."; return }
  # On Windows the Conan CPython package nests python3XX.dll, Lib/ (stdlib) and DLLs/
  # TOGETHER under <prefix>/bin — i.e. the stdlib lives in the DLL's OWN directory
  # (pj_scripting points PYTHONHOME there too). Look there, not one level up.
  $pyBin = $pyDll.Directory.FullName
  foreach ($sub in @("Lib", "DLLs")) {
    $src = Join-Path $pyBin $sub
    if (Test-Path $src) {
      Copy-Item $src $stageBin -Recurse -Force
      Info "staged CPython $sub/ from $src"
    } elseif ($sub -eq "Lib") {
      Warn "CPython Lib/ not found under $pyBin -- pj_scripting may fail to init the interpreter."
    }
  }
  # A pythonXX._pth next to the DLL makes CPython resolve its stdlib RELATIVE TO THE
  # DLL and ignore PYTHONHOME. That is what makes the interpreter portable: the
  # PJ_PYTHON_HOME baked into the binary is the build machine's Conan cache path,
  # which does not exist on the user's machine. Reuse the package's _pth if it ships
  # one; otherwise synthesize a minimal one named to match the DLL (python312.dll ->
  # python312._pth).
  $pth = Get-ChildItem $pyBin -Filter "python3*._pth" -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($pth) {
    Copy-Item $pth.FullName $stageBin -Force
    Info "staged $($pth.Name)"
  } else {
    $pthName = [System.IO.Path]::GetFileNameWithoutExtension($pyDll.Name) + "._pth"
    Set-Content -Path (Join-Path $stageBin $pthName) -Value @(".", "Lib", "DLLs", "import site") -Encoding ASCII
    Info "generated $pthName (portable stdlib path, relative to the DLL)"
  }
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
# Artifact name: <YYYY.MM.DD>.PlotJuggler-<Version>-Windows-x64.<main-commit>.exe
# The commit hash is deliberately taken from MAIN (not the current/checked-out
# branch), so the name pins the artifact to the mainline commit. Resolve local
# `main`, falling back to the `origin/main` tracking ref. (EAP='Continue' so a
# failed rev-parse falls through instead of aborting under the script's 'Stop'.)
$dateStamp = Get-Date -Format 'yyyy.MM.dd'
$prevEAP = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
$mainHash = (& git -C $repoRoot rev-parse --short --verify main 2>$null | Select-Object -First 1)
if (-not $mainHash) { $mainHash = (& git -C $repoRoot rev-parse --short --verify origin/main 2>$null | Select-Object -First 1) }
$ErrorActionPreference = $prevEAP
if (-not $mainHash) { Die "Could not resolve the 'main' commit hash (tried 'main' and 'origin/main' in $repoRoot). Fetch main first: git fetch origin main." }
$mainHash = "$mainHash".Trim()
$outExe = Join-Path (Resolve-Path $OutDir) "$dateStamp.PlotJuggler-$Version-Windows-x64.$mainHash.exe"
Info "running binarycreator -> $outExe"
& $binarycreator --offline-only -c (Join-Path $StageDir "config.xml") -p $stagePackages $outExe
if ($LASTEXITCODE -ne 0) { Die "binarycreator failed (exit $LASTEXITCODE)." }

Info "DONE: $outExe"

# DP_DBStorage: собрать плагин (DLL + объявления), упаковать PBO, разложить по серверу.
#
#   powershell -File deploy.ps1                                        # прод (@dbstorage)
#   powershell -File deploy.ps1 -Game "C:\dayztest\DayZServer" -ModDir "@test"
#
# Сам находит VS-окружение (clang-cl требует VsDevCmd), инструменты из pip
# (py -3.12 -m pip install cmake ninja), LLVM из MSI и MakePbo из Mikero.
param(
    [string] $Game       = "C:/Program Files (x86)/Steam/steamapps/common/DayZServer",
    [string] $ModDir     = "@dbstorage",     # куда: addons/ и grafted/ внутри
    [string] $Preset     = "release",
    [string] $GraftSource = "C:/Grafted",    # локальный клон KR_GRAFTED
    [string] $MakePbo    = "C:/Program Files (x86)/Mikero/DePboTools/bin/makepbo.exe"
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$name = "DP_DBStorage_Server"

# ── окружение: VS dev shell (INCLUDE/LIB для clang-cl) ───────────────────────
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "MSVC не найден (нужен VS Build Tools с C++ workload)" }
$devCmd = Join-Path $vs "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $devCmd)) { throw "VsDevCmd.bat не найден: $devCmd" }

# ── инструменты: cmake/ninja из pip, clang-cl из LLVM MSI ────────────────────
$pyScripts = & py -3.12 -c "import sysconfig; print(sysconfig.get_path('scripts'))"
if (-not (Test-Path (Join-Path $pyScripts "cmake.exe"))) { throw "cmake не найден: $pyScripts (py -3.12 -m pip install cmake ninja)" }
$llvmBin = "C:\Program Files\LLVM\bin"
if (-not (Test-Path (Join-Path $llvmBin "clang-cl.exe"))) { throw "clang-cl не найден: $llvmBin (LLVM MSI)" }
$extraPath = "$llvmBin;$pyScripts"

# ── 1. сборка: DLL -> out/<preset>, объявления -> mod/<name>/scripts ─────────
# ГРАБЛИ (DP_Storage): облако могло отдавать mtime исходника назад, и Ninja
# пропускала перекомпиляцию — принудительно трогаем исходники перед сборкой.
Get-ChildItem "$root\src\*.cpp" | ForEach-Object { $_.LastWriteTime = Get-Date }
cmd /c "`"$devCmd`" -arch=amd64 -host_arch=amd64 && set PATH=$extraPath;%PATH% && cd /d `"$root`" && cmake --preset $Preset -DGRAFT_SOURCE_DIR=$GraftSource && cmake --build --preset $Preset"
if ($LASTEXITCODE) { throw "сборка упала" }

$out = Join-Path $root "out\$Preset"
$mod = Join-Path $root "mod\$name"
if (-not (Test-Path (Join-Path $out "DP_DBStorage.grafted.dll"))) { throw "DLL не собрана: out\$Preset\DP_DBStorage.grafted.dll" }

# ── 2. упаковка PBO ──────────────────────────────────────────────────────────
$dist = Join-Path $root "dist\$ModDir"
New-Item -ItemType Directory -Force "$dist\addons", "$dist\grafted" | Out-Null
& $MakePbo -P -D $mod "$dist\addons\$name.pbo"
if ($LASTEXITCODE) { throw "MakePbo упал" }
Copy-Item -Force (Join-Path $out "DP_DBStorage.grafted.dll") "$dist\grafted\"

# ── 3. деплой: PBO -> addons/, DLL -> grafted/ выбранного мод-каталога ───────
$dstAddons = Join-Path $Game "$ModDir\addons"
$dstGrafted = Join-Path $Game "$ModDir\grafted"
New-Item -ItemType Directory -Force $dstAddons, $dstGrafted | Out-Null
Copy-Item -Force "$dist\addons\$name.pbo" $dstAddons
Copy-Item -Force "$dist\grafted\DP_DBStorage.grafted.dll" $dstGrafted

Write-Host "готово: $Game\$ModDir\addons\$name.pbo + $ModDir\grafted\DP_DBStorage.grafted.dll"
Write-Host "ядро graft ставится один раз: graft install '$Game' (hid.dll); добавить $ModDir в -serverMod="

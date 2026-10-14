param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "build\$Configuration"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio installation with the C++ x64 tools was found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

foreach ($dir in "core", "dll", "tests") { New-Item -ItemType Directory -Force (Join-Path $out $dir) | Out-Null }

$flags = if ($Configuration -eq "Release") { "/O2 /MT /DNDEBUG" } else { "/Od /MTd /Zi /D_DEBUG" }
$common = "/nologo /c /std:c++20 /EHsc /permissive- /utf-8 /Zc:__cplusplus /DNOMINMAX $flags"
$include = "/I`"$root\include`" /I`"$root\src`""

$coreSources = Get-ChildItem (Join-Path $root "src") -Filter *.cpp | Where-Object Name -ne "CApi.cpp" | ForEach-Object { "`"$($_.FullName)`"" }

$script = @"
@echo off
call "$vcvars" >nul || exit /b 1
cl $common /W4 /WX $include /Fo"$out\core\\" $($coreSources -join ' ') || exit /b 1
cl $common /W4 /WX $include /DUDONQUIC_SHARED /DUDONQUIC_EXPORTS /Fo"$out\dll\\" "$root\src\CApi.cpp" || exit /b 1
lib /nologo /OUT:"$out\UdonQuic.Core.lib" "$out\core\*.obj" || exit /b 1
link /nologo /DLL /OUT:"$out\UdonQuic.dll" "$out\dll\CApi.obj" "$out\UdonQuic.Core.lib" || exit /b 1
cl $common /W4 $include /Fo"$out\tests\\" "$root\tests\Tests.cpp" || exit /b 1
link /nologo /OUT:"$out\UdonQuic.Tests.exe" "$out\tests\Tests.obj" "$out\UdonQuic.Core.lib" || exit /b 1
"@

$batch = Join-Path $out "build.cmd"
Set-Content -Path $batch -Value $script -Encoding ascii
& cmd /c "`"$batch`""
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

if (-not $SkipTests) {
    & (Join-Path $out "UdonQuic.Tests.exe")
    if ($LASTEXITCODE -ne 0) { throw "Tests failed." }
}

Write-Host "Built $Configuration into $out"

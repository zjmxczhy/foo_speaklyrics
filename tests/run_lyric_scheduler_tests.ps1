$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$Source = Join-Path $Root "tests\lyric_scheduler_tests.cpp"
$OutputDirectory = Join-Path $Root "build\tests"
$Output = Join-Path $OutputDirectory "lyric_scheduler_tests.exe"

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
cmd /c "`"$Vcvars`" && cl /nologo /W4 /WX /EHsc /std:c++17 /Fe:`"$Output`" `"$Source`""
if ($LASTEXITCODE -ne 0) { throw "lyric_scheduler test build failed." }

& $Output
if ($LASTEXITCODE -ne 0) { throw "lyric_scheduler tests failed." }

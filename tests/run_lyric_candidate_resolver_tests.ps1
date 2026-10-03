$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$Source = Join-Path $Root "tests\lyric_candidate_resolver_tests.cpp"
$OutputDirectory = Join-Path $Root "build\tests"
$Output = Join-Path $OutputDirectory "lyric_candidate_resolver_tests.exe"

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$Command = '"{0}" && cl /nologo /W4 /WX /EHsc /std:c++17 /utf-8 /Fe:"{1}" "{2}"' -f $Vcvars, $Output, $Source
& cmd.exe /d /s /c $Command
if ($LASTEXITCODE -ne 0) { throw "lyric_candidate_resolver test build failed." }

& $Output
if ($LASTEXITCODE -ne 0) { throw "lyric_candidate_resolver tests failed." }

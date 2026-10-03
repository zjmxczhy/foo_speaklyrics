$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$SdkRoot = Join-Path $Root "third_party\foobar2000-sdk"
$Projects = @(Get-ChildItem -LiteralPath $SdkRoot -Filter "*.vcxproj" -Recurse -File | Sort-Object FullName)
if ($Projects.Count -eq 0) {
    throw "No SDK project files were found under $SdkRoot."
}

$Errors = New-Object System.Collections.Generic.List[string]
foreach ($Project in $Projects) {
    try {
        [xml]$Xml = [System.IO.File]::ReadAllText($Project.FullName)
    }
    catch {
        $Errors.Add("Unable to parse $($Project.FullName): $($_.Exception.Message)")
        continue
    }

    $Toolsets = @($Xml.SelectNodes("//*[local-name()='PlatformToolset']") | ForEach-Object { $_.InnerText.Trim() })
    if ($Toolsets.Count -eq 0) {
        $Errors.Add("No PlatformToolset entries found: $($Project.FullName)")
        continue
    }

    $Unexpected = @($Toolsets | Where-Object { $_ -ne "v143" })
    if ($Unexpected.Count -gt 0) {
        $Errors.Add("Unexpected PlatformToolset in $($Project.FullName): $($Unexpected -join ', ')")
    }
    else {
        Write-Host ("OK {0}: {1} configuration(s) use v143." -f $Project.FullName, $Toolsets.Count)
    }
}

if ($Errors.Count -gt 0) {
    $Errors | ForEach-Object { Write-Error $_ }
    throw "SDK PlatformToolset consistency check failed."
}

Write-Host "SDK PlatformToolset consistency check passed: all SDK projects use v143."
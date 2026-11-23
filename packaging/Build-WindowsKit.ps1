param(
    [Parameter(Mandatory=$true)][string]$NeptusHome,
    [Parameter(Mandatory=$true)][string]$JdkHome,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$upstream = (Resolve-Path -LiteralPath $NeptusHome).Path
$jdk = (Resolve-Path -LiteralPath $JdkHome).Path
$expected = '9fe8db6d61ced2f9163023fbedbf689e8eaa55c9'
$actual = git -C $upstream rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actual -ne $expected) { throw "Expected Neptus source $expected." }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Specify a new output directory.' }
$out = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$app = (New-Item -ItemType Directory -Path (Join-Path $out 'app')).FullName

# Export clean upstream resources without copying workstation preferences or logs.
$tracked = Join-Path $out 'upstream-resources.tar'
git -C $upstream archive "--output=$tracked" HEAD -- LICENSE.md legal neptus-config.xml conf/neptus-coordinateSystems.xml conf/neptus-misc-systems.xml conf/mraplots conf/mra-watermark.png
if ($LASTEXITCODE -ne 0) { throw 'Could not export Neptus resources.' }
tar.exe -xf $tracked -C $app
if ($LASTEXITCODE -ne 0) { throw 'Could not extract Neptus resources.' }
Remove-Item -LiteralPath $tracked
New-Item -ItemType Directory -Path (Join-Path $app 'vehicles-defs') | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'neptus-preferences.xml') -Destination (Join-Path $app 'conf/general-properties.xml')
foreach ($entry in 'bin/neptus.jar','plugins/dev.jar') {
    $target = Join-Path $app $entry
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $upstream $entry) -Destination $target
}
foreach ($folder in 'lib','deps') {
    $target = (New-Item -ItemType Directory -Path (Join-Path $app $folder)).FullName
    Get-ChildItem -LiteralPath (Join-Path $upstream $folder) -Filter '*.jar' -File |
        Where-Object { $_.Name -notmatch '^humble-video-arch-|^jogl-all.*natives-(?!windows-amd64)|^xuggle-xuggler|^jmf-' } |
        Copy-Item -Destination $target
}
$jogl = (New-Item -ItemType Directory -Path (Join-Path $app 'lib/jogl2')).FullName
Get-ChildItem -LiteralPath (Join-Path $upstream 'lib/jogl2') -Filter '*.jar' -File |
    Where-Object { $_.Name -notmatch '-natives-(?!windows-amd64)' } |
    Copy-Item -Destination $jogl
foreach ($folder in 'libJNI','libJNI/x64') {
    $target = (New-Item -ItemType Directory -Path (Join-Path $app $folder)).FullName
    Get-ChildItem -LiteralPath (Join-Path $upstream $folder) -Filter '*.dll' -File | Copy-Item -Destination $target
}
foreach ($entry in 'Start-SensorBench.ps1','Start-SensorBench.cmd','SensorBench.ncon','SensorBench.nmis') {
    Copy-Item -LiteralPath (Join-Path $repo "neptus/$entry") -Destination $out
}
$classes = New-Item -ItemType Directory -Path (Join-Path $out '.console-classes')
$sources = @('AlignmentPanel.java','SensorSimPanel.java','InsMonitorPanel.java') | ForEach-Object { Join-Path $repo "source/neptus/$_" }
$cp = @("$upstream/bin/neptus.jar","$upstream/cp.jar","$upstream/plugins/*","$upstream/deps/*","$upstream/lib/*") -join ';'
& (Join-Path $jdk 'bin/javac.exe') --release 11 -encoding UTF-8 -cp $cp -d $classes.FullName @sources
if ($LASTEXITCODE -ne 0) { throw 'Console build failed.' }
& (Join-Path $jdk 'bin/jar.exe') cf (Join-Path $out 'ins-console.jar') -C $classes.FullName .
if ($LASTEXITCODE -ne 0) { throw 'Console jar build failed.' }
$checkedClasses = (Resolve-Path -LiteralPath $classes.FullName).Path
if ($checkedClasses -ne (Join-Path $out '.console-classes')) { throw 'Unexpected class output path.' }
Remove-Item -LiteralPath $checkedClasses -Recurse
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'THIRD-PARTY.md') -Destination $out
& (Join-Path $jdk 'bin/jlink.exe') --add-modules java.se,jdk.crypto.ec,jdk.unsupported,jdk.zipfs,jdk.charsets,jdk.localedata,jdk.management --strip-debug --no-header-files --no-man-pages --compress=2 --output (Join-Path $out 'java')
if ($LASTEXITCODE -ne 0) { throw 'Could not create bundled Java runtime.' }
Write-Output $out

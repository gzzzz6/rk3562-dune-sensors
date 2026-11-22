param(
    [string]$NeptusHome = (Join-Path $PSScriptRoot 'app'),
    [string]$JavaExe
)

$ErrorActionPreference = 'Stop'
if (-not $JavaExe) {
    $bundled = Join-Path $PSScriptRoot 'java/bin/java.exe'
    $JavaExe = if (Test-Path -LiteralPath $bundled) { $bundled } else { 'java.exe' }
}
$root = (Resolve-Path -LiteralPath $NeptusHome).Path
$java = (Get-Command $JavaExe -CommandType Application).Source
$core = Join-Path $root 'bin/neptus.jar'
if (-not (Test-Path -LiteralPath $core -PathType Leaf)) {
    throw 'Point -NeptusHome to an installed or built Neptus directory containing bin/neptus.jar.'
}
$layout = Join-Path $PSScriptRoot 'SensorBench.ncon'
$classpath = @($root, $core, "$root/conf", "$root/cp.jar", "$root/plugins/*", "$root/deps/*", "$root/lib/*") -join ';'
if (Test-Path -LiteralPath (Join-Path $root 'lib/jogl2')) {
    $classpath += ";$root/lib/jogl2/*"
}
$argsList = @('-Xms128m', '-Xmx2g', '--add-opens=java.base/java.lang=ALL-UNNAMED',
    '--add-opens=java.base/java.lang.reflect=ALL-UNNAMED', "-Djava.library.path=$root/libJNI/x64;$root/libJNI",
    '-classpath', $classpath, 'pt.lsts.neptus.loader.NeptusMain', '-f', $layout)
Push-Location -LiteralPath $root
try {
    & $java @argsList
    if ($LASTEXITCODE -ne 0) { throw "Neptus exited with code $LASTEXITCODE" }
} finally {
    Pop-Location
}

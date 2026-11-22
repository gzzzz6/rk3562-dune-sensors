param(
    [Parameter(Mandatory=$true)][string]$NeptusHome,
    [string]$JavaExe = 'java.exe'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $NeptusHome).Path
$java = (Get-Command $JavaExe -CommandType Application).Source
$core = Join-Path $root 'bin/neptus.jar'
if (-not (Test-Path -LiteralPath $core -PathType Leaf)) {
    throw 'Point -NeptusHome to an installed or built Neptus directory containing bin/neptus.jar.'
}
$layout = Join-Path $PSScriptRoot 'SensorBench.ncon'
$classpath = @($root, $core, "$root/conf", "$root/cp.jar", "$root/plugins/*", "$root/deps/*", "$root/lib/*") -join ';'
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

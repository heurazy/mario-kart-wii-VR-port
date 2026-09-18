param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$SourceInstallDirectory,
    [Parameter(Mandatory=$true)][string]$InstallDirectory
)
$ErrorActionPreference='Stop'
function Sha256([string]$Path) {
    $stream=[IO.File]::OpenRead($Path)
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha.Dispose() }
}
$BuildDirectory=[IO.Path]::GetFullPath($BuildDirectory)
$InstallDirectory=[IO.Path]::GetFullPath($InstallDirectory)
$source=Get-Content -LiteralPath (Join-Path $SourceInstallDirectory 'install-state.json') -Raw | ConvertFrom-Json
$target=Get-Content -LiteralPath (Join-Path $InstallDirectory 'install-state.json') -Raw | ConvertFrom-Json
foreach($key in @('DolSha256','RelSha256','RetroRewindCompileInputsSha256','RetroWfcPayloadMode','RetroWfcPayloadSha256','RetroWfcPayloadLength')) {
    if($source.$key -ne $target.$key) { throw "Incompatible build inputs: $key" }
}
# This developer-only publisher accepts known local build output, never an arbitrary
# downloaded executable. Keep the installer's integrity checks enabled.
if(!(Test-Path -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt'))) {throw 'Expected a local native build directory'}
$plan=@()
foreach($product in @('WiiCompiled','RetroRewind')) {
    if(Get-Process -Name $product -ErrorAction SilentlyContinue) {throw "Close $product before deploying"}
    $folder=if($product -eq 'WiiCompiled') {'Base'} else {'RetroRewind'}
    $built=Join-Path $BuildDirectory "$product.exe"
    $exe=Join-Path $InstallDirectory "$folder\$product.exe"
    $metadata=Join-Path $InstallDirectory "$folder\build-fingerprint.json"
    $record=Get-Content -LiteralPath $metadata -Raw | ConvertFrom-Json
    if($record.DolSha256 -ne $source.DolSha256 -or $record.RelSha256 -ne $source.RelSha256) {throw 'Product inputs mismatch'}
    $record.ExecutableSha256=Sha256 $built
    $record.BuiltUtc=(Get-Item -LiteralPath $built).LastWriteTimeUtc.ToString('o')
    $record | Add-Member -Force NoteProperty LocalHotfixBuildDirectory $BuildDirectory
    $record | Add-Member -Force NoteProperty LocalHotfixCMakeCacheSha256 (Sha256 (Join-Path $BuildDirectory 'CMakeCache.txt'))
    $plan+=@{Built=$built;Exe=$exe;Metadata=$metadata;Record=$record}
}
foreach($item in $plan) {
    foreach($path in @($item.Exe,$item.Metadata)) {
        if(!(Test-Path -LiteralPath "$path.before-local-hotfix")) {Copy-Item -LiteralPath $path -Destination "$path.before-local-hotfix"}
    }
    Copy-Item -LiteralPath $item.Built -Destination $item.Exe
    if((Sha256 $item.Exe) -ne $item.Record.ExecutableSha256) {throw 'Published executable verification failed'}
    $temp="$($item.Metadata).hotfix-pending"
    [IO.File]::WriteAllText($temp,($item.Record | ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
    [IO.File]::Replace($temp,$item.Metadata,"$($item.Metadata).previous-hotfix")
}
Write-Output 'Local executables and build identities published together. Run setup --check-products to verify.'

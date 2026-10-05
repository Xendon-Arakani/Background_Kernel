[CmdletBinding()]
param(
    [string]$OutRoot = '',
    [string]$Version = '0.1.0'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutRoot) {
    $OutRoot = Join-Path $root 'release'
}
$out = [IO.Path]::GetFullPath($OutRoot)
$build = Join-Path $root 'build-release'
$name = "Background_Kernel-$Version-win64"
$stage = Join-Path $out $name
$zip = Join-Path $out "$name.zip"
$source = Join-Path $out "Background_Kernel-$Version-source.zip"
$repo = Test-Path -LiteralPath (Join-Path $root '.git')
if ($repo) {
    $state = & git -C $root status --porcelain
    if ($LASTEXITCODE -or $state) { throw 'source_dirty' }
    $commit = (& git -C $root rev-parse HEAD).Trim()
    if ($LASTEXITCODE -or -not $commit) { throw 'commit_missing' }
}
else {
    $commit = 'archive'
}

foreach ($path in @($build,$stage)) {
    $full = [IO.Path]::GetFullPath($path)
    if ($full -ne $root -and
        ($full.StartsWith([IO.Path]::GetFullPath($root),[StringComparison]::OrdinalIgnoreCase) -or
         $full.StartsWith($out,[StringComparison]::OrdinalIgnoreCase)) -and
        (Test-Path -LiteralPath $full)) {
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}
New-Item -ItemType Directory -Path $out -Force | Out-Null

& cmake -S $root -B $build -A x64
if ($LASTEXITCODE) { throw 'configure_failed' }
& cmake --build $build --config Release --parallel
if ($LASTEXITCODE) { throw 'build_failed' }
& ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'policy_failed' }
& cmake --install $build --config Release --component Runtime --prefix $stage
if ($LASTEXITCODE) { throw 'install_failed' }

$entries = Get-ChildItem -LiteralPath $stage -File | Sort-Object Name | ForEach-Object {
    [ordered]@{
        path = $_.Name
        bytes = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$allow = Join-Path $stage 'files.json'
$allowText=[ordered]@{version=$Version;commit=$commit;files=@($entries)} |
    ConvertTo-Json -Depth 4
[IO.File]::WriteAllText($allow,$allowText,[Text.UTF8Encoding]::new($false))

if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal
$artifacts = @($zip)
if ($repo) {
    if (Test-Path -LiteralPath $source) { Remove-Item -LiteralPath $source -Force }
    & git -C $root archive --format=zip --output=$source HEAD
    if ($LASTEXITCODE) { throw 'source_pack_failed' }
    $artifacts += $source
}

$hashes = $artifacts | ForEach-Object {
    $item = Get-Item -LiteralPath $_
    [ordered]@{
        path = $item.FullName
        bytes = $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$hashText=[ordered]@{version=$Version;commit=$commit;artifacts=$hashes} |
    ConvertTo-Json -Depth 4
[IO.File]::WriteAllText(
    (Join-Path $out 'hashes.json'),$hashText,[Text.UTF8Encoding]::new($false))

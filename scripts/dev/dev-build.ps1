<#
.SYNOPSIS
  Build rapido do OrcaSlicer para testar alteracoes de codigo.

.DESCRIPTION
  Compila apenas o app (alvo OrcaSlicer_app_gui, que puxa o OrcaSlicer.dll),
  pula gettext e o install, e copia o orca-slicer.exe + OrcaSlicer.dll
  recem-gerados para a pasta executavel (build\OrcaSlicer), que ja contem
  'resources' e as DLLs das dependencias.

  Por padrao usa o diretorio de build do Visual Studio (build\) -> ganho
  imediato, sem recompilar tudo de novo.

  Com -Ninja usa build-ninja\ (gerador Ninja Multi-Config), que tem builds
  incrementais mais rapidos quando MUITOS arquivos mudam (ex.: mexer num
  header compartilhado). Requer 1 build completo inicial (a 1a vez demora).

.EXAMPLE
  .\scripts\dev\dev-build.ps1            # compila (VS) e copia o binario
  .\scripts\dev\dev-build.ps1 -Run       # compila, copia e abre o app
  .\scripts\dev\dev-build.ps1 -Ninja -Run
  .\scripts\dev\dev-build.ps1 -Ninja -Configure   # (re)configura o build-ninja
#>
param(
    [switch]$Run,
    [switch]$Ninja,
    [switch]$Configure,
    [switch]$Restart
)
$ErrorActionPreference = 'Stop'
$repo   = (Get-Item $PSScriptRoot).Parent.Parent.FullName
$runDir = Join-Path $repo 'build\OrcaSlicer'
$target = 'OrcaSlicer_app_gui'
$env:CMAKE_POLICY_VERSION_MINIMUM = '3.5'

if (-not (Test-Path $runDir)) {
    throw "Pasta executavel nao encontrada: $runDir`nRode um build completo + install uma vez antes de usar este script."
}

if ($Ninja) {
    $bdir   = Join-Path $repo 'build-ninja'
    $vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat nao encontrado: $vcvars" }
    # Importa o ambiente do MSVC (cl.exe/link.exe/INCLUDE/LIB) para o Ninja
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
    }
    if ($Configure -or -not (Test-Path (Join-Path $bdir 'CMakeCache.txt'))) {
        Write-Host '==> Configurando build-ninja (Ninja Multi-Config)...' -ForegroundColor Cyan
        cmake -S $repo -B $bdir -G 'Ninja Multi-Config' -DORCA_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release
        if ($LASTEXITCODE -ne 0) { throw "Configure falhou (exit $LASTEXITCODE)" }
    }
} else {
    $bdir = Join-Path $repo 'build'
    if (-not (Test-Path (Join-Path $bdir 'CMakeCache.txt'))) {
        throw "build\ nao esta configurado. Use -Ninja ou rode o build VS completo primeiro."
    }
}

Write-Host "==> Compilando alvo '$target' em '$([IO.Path]::GetFileName($bdir))'..." -ForegroundColor Cyan
$sw = [Diagnostics.Stopwatch]::StartNew()
cmake --build $bdir --config Release --target $target
if ($LASTEXITCODE -ne 0) { throw "Build falhou (exit $LASTEXITCODE)" }
$sw.Stop()

# O exe/dll ficam travados se houver uma instancia aberta a partir desta pasta.
$dev = Get-Process -Name 'orca-slicer' -ErrorAction SilentlyContinue |
       Where-Object { $_.Path -and $_.Path.StartsWith($repo, [StringComparison]::OrdinalIgnoreCase) }
if ($dev) {
    if ($Run -or $Restart) {
        Write-Host '==> Fechando instancia aberta para liberar o binario...' -ForegroundColor Yellow
        $dev | Stop-Process -Force
        for ($i = 0; $i -lt 20 -and (Get-Process -Name 'orca-slicer' -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($repo, [StringComparison]::OrdinalIgnoreCase) }); $i++) {
            Start-Sleep -Milliseconds 150
        }
    } else {
        throw "O OrcaSlicer esta aberto (trava o .exe/.dll). Feche-o e rode de novo, ou use -Run (fecha e reabre) / -Restart (so fecha)."
    }
}

Write-Host '==> Copiando binarios para a pasta executavel...' -ForegroundColor Cyan
foreach ($name in @('orca-slicer.exe', 'OrcaSlicer.dll')) {
    # Procura so em <build>\src para nao pegar a copia ja existente em build\OrcaSlicer
    $src = Get-ChildItem (Join-Path $bdir 'src') -Recurse -Filter $name -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $src) { throw "Nao encontrei $name em $bdir\src" }
    Copy-Item $src.FullName (Join-Path $runDir $name) -Force
    Write-Host "    $name"
}

Write-Host ("==> Pronto em {0:N1}s. Exe: {1}" -f $sw.Elapsed.TotalSeconds, (Join-Path $runDir 'orca-slicer.exe')) -ForegroundColor Green

if ($Run) {
    Write-Host '==> Abrindo o OrcaSlicer...' -ForegroundColor Cyan
    Start-Process (Join-Path $runDir 'orca-slicer.exe')
}

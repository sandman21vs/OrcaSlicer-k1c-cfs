# Build local no Windows (passo a passo)

Esta máquina não tem compilador C++ instalado. Para compilar o fork e testar o sync de CFS na K1C:

## 1. Pré-requisitos (instalar uma vez)

- **Visual Studio 2022 Community** com o workload **"Desenvolvimento para desktop com C++"**
  (https://visualstudio.microsoft.com/). Esse workload traz MSVC, Windows SDK e MSBuild.
- **CMake** — já instalado (`C:\Program Files\CMake`).
- **Git** — já instalado.
- Espaço em disco: **~30–40 GB** livres. Tempo: a 1ª compilação das *deps* leva ~1–2 h; o app, ~30–60 min.

> Não precisa instalar mais nada: as deps são baixadas/compiladas pelo próprio script.

## 2. Compilar (1ª vez — deps + app)

Abra o **Prompt de Comando** (cmd) normal e rode:

```bat
cd C:\Users\vinic\Documents\GitHub\OrcaSlicer-k1c-cfs
build_release_vs2022.bat
```

O script faz tudo: compila as deps em `deps\build`, depois o app em `build`, roda o gettext e instala.
As mudanças do CFS já estão na árvore de trabalho (não precisa commitar para compilar).

## 3. Onde fica o .exe

Ao terminar:

```
C:\Users\vinic\Documents\GitHub\OrcaSlicer-k1c-cfs\build\OrcaSlicer\OrcaSlicer.exe
```

É uma versão **portátil** (roda dessa pasta). Use ela para testar — não substitui o seu OrcaSlicer normal.

## 4. Recompilar depois (rápido — só o código mudou)

Se você editar só código/perfis depois da 1ª build completa, não precisa refazer as deps:

```bat
cd C:\Users\vinic\Documents\GitHub\OrcaSlicer-k1c-cfs
build_release_vs2022.bat slicer
```

(o argumento `slicer` pula a etapa de deps). Ou, mais direto, reabrir e buildar o alvo:

```bat
cmake --build build --config Release --target ALL_BUILD -- -m
```

## 5. Teste

Siga o [04-verification.md](04-verification.md): selecione o perfil "Creality K1C_CFS-C", conecte a impressora,
clique no sync e confira os slots (com seu CFS atual: T1A Hyper ABS preto, T1B CR-PLA Matte `#0B359A`,
T1C CR-PLA `#F8E911`, T1D Hyper PLA `#FA7C0C`). Nos logs procure
`CrealityPrintAgent: CFS (Klipper box) with 4 loaded slot(s)`.

## Problemas comuns

- **"Could not determine Visual Studio version" / cmake não acha o gerador**: confirme o workload C++ do VS2022 e
  rode num cmd novo (para o PATH atualizar).
- **Build de dep falha por download**: rode de novo o script (ele retoma) e cheque a conexão.
- **Falta de disco**: a pasta `deps\build` e `build` somam dezenas de GB.

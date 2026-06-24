# Build rápido para testes (dev)

`dev-build.ps1` acelera o ciclo *editar → compilar → testar*. Em vez de recompilar
tudo (`ALL_BUILD` + `gettext` + `install`), ele:

1. compila **só o app** (alvo `OrcaSlicer_app_gui`, que puxa o `OrcaSlicer.dll`);
2. **pula** gettext e install;
3. copia o `orca-slicer.exe` + `OrcaSlicer.dll` novos para `build\OrcaSlicer\`
   (que já tem `resources` e as DLLs das dependências da 1ª compilação completa).

## Pré-requisito (uma vez)

Ter feito um build completo + install antes, para existir a pasta
`build\OrcaSlicer\` (executável + resources + DLLs). É o que já temos.

## Uso

```powershell
# Compila (Visual Studio) e copia o binário — ganho imediato
.\scripts\dev\dev-build.ps1

# Compila, copia e já abre o app
.\scripts\dev\dev-build.ps1 -Run
```

### Modo Ninja (turbo para mudanças grandes)

Usa um diretório separado `build-ninja\` com o gerador **Ninja Multi-Config**,
que tem incremental mais rápido quando muitos arquivos mudam (ex.: header
compartilhado). A **1ª vez** faz um build completo (demorado); depois é rápido.

```powershell
# 1ª vez: configura + build completo (longo)
.\scripts\dev\dev-build.ps1 -Ninja -Configure

# Depois, a cada alteração (rápido):
.\scripts\dev\dev-build.ps1 -Ninja -Run
```

## Quando ainda preciso do build completo?

- Mudou **tradução** (`.po`/`.mo`) → rode `scripts\run_gettext.bat`.
- Mudou algo em **`resources\`** (perfis, ícones) → rode o `install` completo
  (`cmake --build build --target install --config Release`) ou copie o arquivo
  manualmente para `build\OrcaSlicer\resources\`.
- Vai **gerar o exe oficial** de distribuição → use o build Release completo.

Para teste de comportamento de código, o `dev-build.ps1` basta.

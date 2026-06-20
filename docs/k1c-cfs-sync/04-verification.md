# Verificação — testar o sync de CFS na K1C

## 0. Descoberta (antes de confiar no resultado)

Rode os passos de [03-diagnostics.md](03-diagnostics.md) e confirme o transporte/schema do CFS na sua K1C.

## 1. Build (Windows)

```
cmake --build . --config RelWithDebInfo --target ALL_BUILD -- -m
```

## 2. Roteamento do agente

1. No OrcaSlicer, selecione o perfil de impressora **"Creality K1C_CFS-C"**.
2. Conecte/registre a impressora (IP local).
3. Confirme no log: `switch_printer_agent: printer agent switched to crealityprint`.

## 3. Sync de filamento (caminho feliz)

1. Carregue filamentos no CFS (spools com tag NFC).
2. Clique no botão de sync de filamento (AMS) no painel lateral.
3. Confirme nos logs:
   - `CrealityPrintAgent: CFS (Klipper box) with M loaded slot(s) across N box(es)`  (caminho K1C)
   - ou, em K2, o caminho WS:9999 pré-existente.
4. Confirme na UI: os slots de filamento do Orca recebem **cor** e **tipo** corretos de cada slot do CFS.
   Para a captura de referência da K1C, espere: T1A ABS preto, T1B PLA `#0B359A`, T1C PLA `#F8E911`,
   T1D PLA `#FA7C0C`.

## 4. Matching de preset

- K1C: o `filamentId` do objeto `box` é resolvido pelo catálogo embutido para marca/produto e casado via
  `match_filament_preset()`. Esperado com o CFS atual: T1A→**Hyper ABS**, T1B→**CR-PLA Matte**,
  T1C→**CR-PLA**, T1D→**Hyper PLA** (todos Creality). Código desconhecido → fallback genérico por tipo.
- K2: continua usando `match_filament_preset()` (marca/vendor via `boxsInfo`).
- Cores K1C: chegam como `"0RRGGBB"` (7 hex). Confirme que aparecem como `#RRGGBB` correto na UI.

## 5. Sem regressão

- **K2 / K2 Plus / K2 Pro**: o sync continua funcionando (mesmo caminho, agora dinâmico; `box_count > 0`).
- **K1C sem CFS** (perfil normal) e outras impressoras Moonraker: o fetch faz fallback sem erro, sem travar o botão.
- **Impressão normal** (upload/start_print) na K1C continua igual — não tocamos `supports_multi_color_print()`.

## 6. Critérios de aceite

- [ ] Botão de sync popula os slots na K1C com os filamentos reais do CFS.
- [ ] Cores e tipos corretos; presets casados de forma sensata.
- [ ] K2 e demais impressoras sem regressão.
- [ ] Logs mostram o caminho `crealityprint` + contagem de caixas/slots.

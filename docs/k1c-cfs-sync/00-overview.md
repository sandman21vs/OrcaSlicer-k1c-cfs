# Sync automático de filamento (CFS) na Creality K1C — Visão geral

## Problema

Uma **Creality K1C com CFS** conectada localmente ao OrcaSlicer. Os spools usam **tag NFC**, então o CFS e a
impressora sempre sabem qual filamento está carregado. No **Creality Print**, o botão de sync automático puxa
essas informações do CFS e aplica nos slots de filamento do slicer — localmente, sem nuvem. No **OrcaSlicer**
(este fork `OrcaSlicer-k1c-cfs`), o mesmo botão **não funciona** para a K1C.

## Conclusão da análise

O fork **já tem** quase toda a infraestrutura de sync de CFS — ela foi escrita para a família **K2** e
simplesmente **não é ativada para a K1C**. Não é um recurso ausente; são três pontos de gating que excluem a K1C.

Veja [01-diagnosis.md](01-diagnosis.md) para o detalhamento, [02-action-plan.md](02-action-plan.md) para as
mudanças, [03-diagnostics.md](03-diagnostics.md) para a coleta de dados da impressora e
[04-verification.md](04-verification.md) para os testes.

## Como funciona no Creality Print (referência)

Pasta analisada: `C:\Users\vinic\Documents\GitHub\CrealityPrint`.

1. Botão "Auto Mapping" → `PrinterBoxFilamentPanel::on_auto_device_filament_mapping()`.
2. Para dispositivo local, dispara `EVT_AUTO_SYNC_CURRENT_DEVICE_FILAMENT` → `FilamentPanel::on_auto_mapping_filament()`.
3. Os dados do dispositivo já vêm de chamadas **HTTP `GET http://<ip>/info`**, que retorna um objeto
   `boxsInfo` com `materialBoxs[]` e, em cada um, `materials[]` (campos `vendor`, `type`, `name`, `rfid`, `color`, ...).
4. O matching aplica cor/tipo/nome aos slots, casando por `vendor` + `type` + prefixo do `name` (case-insensitive).

Estruturas: `Material`, `MaterialBox`, `Device` em `src/slic3r/GUI/print_manage/data/DataType.hpp`.

## Como funciona (ou deveria) no OrcaSlicer

Fluxo do botão de sync:

```
Sidebar::sync_ams_list()            src/slic3r/GUI/Plater.cpp:3500
  -> Sidebar::load_ams_list()       src/slic3r/GUI/Plater.cpp:3466
    -> Sidebar::build_filament_ams_list()   src/slic3r/GUI/Plater.cpp:3335
       (se agent->get_filament_sync_mode() == pull) -> agent->fetch_filament_info()
```

Caminho específico de Creality:

- `CrealityPrintAgent` (herda `MoonrakerPrinterAgent`) — `src/slic3r/Utils/CrealityPrintAgent.{hpp,cpp}`
- **K1-series (K1C)**: `CrealityPrintAgent::fetch_cfs_box_object()` — lê o objeto Klipper `box` via Moonraker
  (`/printer/objects/query?box`) e parseia `same_material`. **Adicionado** nesta correção.
- **K2 e família**: `CrealityPrint::query_boxes_info()` (WebSocket porta 9999, `src/slic3r/Utils/CrealityPrint.cpp:284`)
  + `CrealityPrintAgent::parse_cfs_response()` (schema `boxsInfo.materialBoxs[].materials[]`) +
  `match_filament_preset()` (casa marca/vendor por pontuação). Pré-existente, intacto.
- `MoonrakerPrinterAgent::build_ams_payload()` — publica os trays no `DevFilaSystem` (comum aos dois caminhos).

> Importante: o `boxsInfo`/WS:9999 descrito na referência do Creality Print é da **K2**. A **K1C** expõe o CFS
> só pelo objeto `box` do Moonraker (ver [03-diagnostics.md](03-diagnostics.md)).

## Mapa de arquivos relevantes

| Arquivo | Papel |
|---|---|
| `src/slic3r/Utils/CrealityPrintAgent.cpp` | Fetch de CFS + matching de preset (gate K2-only a remover) |
| `src/slic3r/Utils/CrealityPrint.cpp` | Cliente WS/HTTP; `supports_multi_color_print()` (allowlist K2) |
| `src/slic3r/Utils/MoonrakerPrinterAgent.cpp` | Base do agente; `build_ams_payload()`; modo `pull` |
| `src/slic3r/Utils/NetworkAgentFactory.cpp` | Registro dos agentes (`crealityprint` antes de `moonraker`) |
| `src/slic3r/GUI/GUI_App.cpp` | `switch_printer_agent()` — escolhe agente por `printer_agent` do perfil |
| `src/slic3r/GUI/Plater.cpp` | Fluxo do botão de sync |
| `resources/profiles/Creality/machine/Creality K1C_CFS-C 0.4 nozzle.json` | Perfil da K1C com CFS (falta `printer_agent`) |

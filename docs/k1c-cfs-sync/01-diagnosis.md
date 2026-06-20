# Diagnóstico — por que o sync de CFS não funciona na K1C

Três lacunas, todas verificadas no código. As duas primeiras já bastam para o botão não fazer nada útil na K1C.

## Lacuna 1 — Roteamento de agente: a K1C nunca usa o `CrealityPrintAgent`

- `GUI_App::switch_printer_agent()` (`src/slic3r/GUI/GUI_App.cpp:3462`) escolhe o agente pela opção de perfil
  `printer_agent`. Sem ela, usa o padrão `"orca"` (`ORCA_PRINTER_AGENT_ID`).
- `"printer_agent": "crealityprint"` está definido **só** para K2 / K2 Plus / K2 Pro / SPARKX i7
  (`resources/profiles/Creality/machine/Creality K2*.json`, `Creality SPARKX i7*.json`).
- O perfil `resources/profiles/Creality/machine/Creality K1C_CFS-C 0.4 nozzle.json` **não** tem `printer_agent`
  (e tem `"host_type": "octoprint"`).
- Resultado: a K1C cai no `OrcaPrinterAgent`, cujo `get_filament_sync_mode()` herda o default `none`
  (`src/slic3r/Utils/IPrinterAgent.hpp:270`).
- Em `Sidebar::build_filament_ams_list()` (`src/slic3r/GUI/Plater.cpp:3342`), o fetch só roda quando o modo é
  `pull`:

  ```cpp
  auto* agent = wxGetApp().getDeviceManager()->get_agent();
  if (agent && agent->get_filament_sync_mode() == FilamentSyncMode::pull) {
      if (!agent->fetch_filament_info(obj->get_dev_id())) {
          return filament_ams_list;   // vazio
      }
  }
  ```

  Como o modo é `none`, **nada é buscado**, a lista volta vazia e o usuário vê o aviso "não conectado".

## Lacuna 2 — Detecção de modelo K2-only no fetch

Mesmo que a K1C fosse roteada para o `CrealityPrintAgent`, o CFS não seria consultado:

- `CrealityPrintAgent::fetch_filament_info()` (`src/slic3r/Utils/CrealityPrintAgent.cpp:257`) desiste cedo:

  ```cpp
  if (!host.supports_multi_color_print()) {
      // ... "is not CFS-capable, deferring to base Moonraker agent"
      return MoonrakerPrinterAgent::fetch_filament_info(std::move(dev_id));
  }
  ```

- `CrealityPrint::supports_multi_color_print()` (`src/slic3r/Utils/CrealityPrint.cpp:259`) só reconhece a
  família K2:

  ```cpp
  return m_model == "F008"    // K2 Plus
      || m_model == "F012"    // K2 Pro
      || m_model == "F021"    // K2
      || m_model == "F022";   // SPARKX i7
  ```

  A K1C reporta outro código de `model` → cai no Moonraker base, que não faz sync de CFS.

## Lacuna 3 — Transporte do CFS é DIFERENTE na K1C (confirmado por captura)

Capturado diretamente da K1C do usuário (`192.168.100.13`, firmware `1.0.0`, box `1.1.3`) em 2026-06-20:

- `GET http://IP/info` na K1C retorna **só** `{"mac","model":"K1C","version"}` — **não** tem `boxsInfo`.
  (Diferente da K2, onde o `/info`/WS expõe `boxsInfo`.)
- A K1C roda **Klipper + Moonraker** (porta `7125`). O CFS é exposto como o **objeto Klipper `box`**, lido via
  `GET http://IP:7125/printer/objects/query?box`. O WS proprietário porta 9999 (usado pela K2) **não** se aplica.
- Schema real do objeto `box` (resumo):
  - `same_material`: lista de `[type_code, "0RRGGBB", [labels "T1A"...], "PLA"|"ABS"|...]` — os slots carregados.
  - `T1`..`T4`: uma caixa cada (até 4), com arrays paralelos `material_type`, `color_value`, `remain_len` e
    `state` (`"connect"` quando a caixa está presente). Slots A–D dentro de cada caixa.
  - Cor vem como `"0RRGGBB"` (7 hex, com 0 à esquerda).
- Portanto, o caminho existente (`CrealityPrint::query_boxes_info()` via WS:9999 +
  `parse_cfs_response()` do schema `boxsInfo`) **não funciona para a K1C** — é específico da K2. Era preciso um
  parser novo para o objeto `box`. Ver a implementação na seção B/C do [02-action-plan.md](02-action-plan.md).

## Sintoma observado pelo usuário

Botão de sync não traz os filamentos do CFS na K1C, enquanto o Creality Print faz isso perfeitamente —
consistente com as Lacunas 1 e 2 (o caminho de fetch nunca é executado para a K1C).

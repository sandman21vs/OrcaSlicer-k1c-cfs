# Plano de ação — mudanças de código (IMPLEMENTADO)

Abordagem: **detecção dinâmica de CFS**. Após a descoberta ([03-diagnostics.md](03-diagnostics.md)) ficou claro
que a **K1C usa um transporte diferente da K2**: o CFS é o objeto Klipper `box` via Moonraker, não o
WebSocket porta 9999 `boxsInfo`. Por isso a correção tem 3 partes.

## A. Roteamento do agente para a K1C  ✅ implementado

Adicionado `"printer_agent": "crealityprint"` aos perfis de máquina da K1C:

- `resources/profiles/Creality/machine/Creality K1C_CFS-C 0.4 nozzle.json`
- `resources/profiles/Creality/machine/Creality K1C 0.4 nozzle.json`
- `resources/profiles/Creality/machine/Creality K1C 0.6 nozzle.json`
- `resources/profiles/Creality/machine/Creality K1C 0.8 nozzle.json`

```diff
  "printer_model": "Creality K1C",
  "gcode_flavor": "klipper",
+ "printer_agent": "crealityprint",
```

Sem isso, a K1C usava o agente "orca" (modo `none`) e o fetch de CFS nunca rodava. É aditivo e seguro:
`CrealityPrintAgent` herda 100% do `MoonrakerPrinterAgent`; sem CFS, comporta-se igual ao Moonraker.

## B. Detecção dinâmica (remoção do gate por modelo K2)  ✅ implementado

Arquivo: `src/slic3r/Utils/CrealityPrintAgent.cpp`, `fetch_filament_info()`.

Removido o gate `if (!host.supports_multi_color_print()) return base;` (allowlist F008/F012/F021/F022). Agora o
fetch tenta detectar o CFS e o critério é "vieram slots carregados", não o código de modelo. **Não** alteramos
`supports_multi_color_print()`, pois ela governa o caminho de impressão multicor em `upload()`/`start_print()`
(`src/slic3r/Utils/CrealityPrint.cpp:146,340`) — fora do escopo do sync.

## C. Parser do objeto `box` do Moonraker (K1-series)  ✅ implementado

Arquivos: `src/slic3r/Utils/CrealityPrintAgent.{hpp,cpp}`.

Novo método privado `fetch_cfs_box_object(trays, max_slot_index)` que:

1. Faz `GET /printer/objects/query?box` reusando `Http::get` + `join_url` + `X-Api-Key` do agente base.
   Como a porta 80 da K1C é a API da Creality (não o Moonraker), tenta as portas do Moonraker em ordem:
   `:7125` (direto) → `:4409` → `:4408` (proxy nginx) → `base_url`, usando a primeira que devolver o objeto `box`.
2. Lê `result.status.box.same_material` — cada entrada `[type_code, "0RRGGBB", [labels], type]`.
3. Para cada label `"T<box><slot>"` (box 1..4, slot A..D) calcula `slot_index = (box-1)*4 + (slot-'A')`.
4. Normaliza a cor (mantém os 6 hex finais de `"0RRGGBB"`) e o tipo (`normalize_filament_type`).
5. **Resolve a marca/produto** pelo `filamentId` (`same_material[0]`) usando um catálogo embutido
   `código → (vendor, nome, tipo)` e casa o preset exato via `match_filament_preset()` (pontuação por marca/vendor),
   com fallback para `filament_id_by_type` quando o código é desconhecido.
6. Publica via `build_ams_payload()` (mesmo pipeline da K2/Qidi/Snapmaker).

`fetch_filament_info()` agora tenta, em ordem:
1. **objeto `box` do Moonraker** (K1-series, ex.: K1C) — `fetch_cfs_box_object()`;
2. **WS:9999 `boxsInfo`** (K2 e família) — caminho pré-existente, intacto;
3. **Moonraker base** (`lane_data` / Happy Hare) — fallback.

Assim a K1C funciona pelo passo 1 e a K2 continua funcionando pelo passo 2, sem regressão.

## D. (Opcional, fora do escopo) Impressão multicor na K1C

O botão de *imprimir* multicor (≠ sync) precisaria de `supports_multi_color_print()` reconhecendo a K1C, além de
um caminho de `start_print` compatível com o transporte da K1C. Item separado; não implementado agora.

## Catálogo de materiais (resolução de marca)

Tabela embutida em `CrealityPrintAgent.cpp` (`cfs_catalog()`), 66 entradas, extraída da base de materiais da
própria Creality (`/mnt/UDISK/creality/userdata/box/material_database.json`, espelhada no projeto comunitário
K2-RFID em `db/{k1,k2,hi}.json`). Chave = id de catálogo de 5 dígitos (os 5 últimos do `filamentId`); valor =
`(vendor, nome do produto, tipo)`. Sem conflitos entre as três bases.

- Validado contra o CFS do usuário: `103001`→Hyper ABS, `114001`→CR-PLA Matte, `104001`→CR-PLA, `101001`→Hyper PLA.

## Limitações conhecidas

- **Catálogo estático**: se a Creality lançar um material novo com código ainda não listado, o slot cai no
  fallback por tipo (genérico). Atualizar = regerar a tabela a partir do `material_database.json` mais recente.
- `remain_len` (% restante) é lido pela impressora mas não é propagado aos slots do Orca (o Orca não exibe % de
  CFS hoje).

# Descoberta — dados capturados da K1C (2026-06-20)

Impressora do usuário: `192.168.100.13` (K1C com CFS, spools com tag NFC).

## 1. `GET http://IP/info`

```json
{ "mac": "FCEE28035E67", "model": "K1C", "version": "1.0.0" }
```

Conclusão: o `/info` da K1C **não** traz `boxsInfo` (diferente da K2). `model` = `"K1C"`.

## 2. Moonraker ativo — atenção às portas

- `GET http://IP:7125/server/info` → `klippy_state: ready`, Moonraker `v0.10.0`.
- **Porta 80** (`http://IP/...`) NÃO é o Moonraker — é a API de controle da Creality (`/info`); endpoints
  Moonraker dão **404** nela.
- **Porta 7125** = Moonraker direto (200). **Porta 4409** (e geralmente 4408) = nginx proxiando Moonraker (200).

Consequência para o código: o `device_info.base_url` do agente é `http://IP` (porta 80), que **não** serve
Moonraker. Por isso o `fetch_cfs_box_object()` tenta, em ordem, `:7125` → `:4409` → `:4408` → `base_url`, e usa
a primeira que devolver o objeto `box`.

## 3. Lista de objetos do Klipper (`/printer/objects/list`)

Entre os 165 objetos, apareceu **`box`** (e macros `BOX_INFO_REFRESH`, `BOX_CHECK_MATERIAL`,
`BOX_LOAD_MATERIAL_*`). É aqui que o CFS está.

## 4. Estado do CFS — `GET http://IP:7125/printer/objects/query?box`

Resposta real (resumida) com 4 spools na caixa 1:

```json
"box": {
  "filament": 1, "state": "connect", "enable": 1,
  "same_material": [
    ["103001","0000000",["T1A"],"ABS"],
    ["114001","00B359A",["T1B"],"PLA"],
    ["104001","0F8E911",["T1C"],"PLA"],
    ["101001","0FA7C0C",["T1D"],"PLA"]
  ],
  "T1": {
    "state": "connect", "filament": "A",
    "material_type": ["103001","114001","104001","101001"],
    "color_value":   ["0000000","00B359A","0F8E911","0FA7C0C"],
    "remain_len":    ["41","29","100","41"],
    "vender": ["AB1240276A21030010...", ...]
  },
  "T2": { "state": "None", ... }, "T3": {...}, "T4": {...}
}
```

Decodificação:

| Slot | type_code | Tipo | Cor (`0RRGGBB` → `#RRGGBB`) | Restante |
|------|-----------|------|------|----------|
| T1A | 103001 | ABS | `0000000` → `#000000` | 41% |
| T1B | 114001 | PLA | `00B359A` → `#0B359A` | 29% |
| T1C | 104001 | PLA | `0F8E911` → `#F8E911` | 100% |
| T1D | 101001 | PLA | `0FA7C0C` → `#FA7C0C` | 41% |

Notas de schema usadas na implementação:
- **Posição**: o label `T<box><slot>` em `same_material` dá caixa (1..4) e slot (A..D). `slot_index = (box-1)*4 + (slot-'A')`.
- **Cor**: 7 hex com 0 à esquerda → manter os 6 finais.
- **Tipo**: string em `same_material[3]` (PLA/ABS/...). `material_type` (código numérico) é o ID Creality do produto.
- **Marca/produto**: NÃO vem em texto; só o `type_code` e o `vender` (tag NFC bruta, 40 hex). Por isso o matching
  é por tipo base (ver Limitações no [02-action-plan.md](02-action-plan.md)).

## 5. Como reproduzir (qualquer K-series Klipper)

```bash
curl -s http://IP/info
curl -s http://IP:7125/printer/objects/list   # procure por "box"
curl -s "http://IP:7125/printer/objects/query?box"   # estado do CFS
```

## 6. Logs do OrcaSlicer (após o build)

- `switch_printer_agent: printer agent switched to crealityprint`
- `CrealityPrintAgent: CFS (Klipper box) with M loaded slot(s) across N box(es)`
- (se não houver objeto `box`) cai para WS:9999 / Moonraker base, sem erro.

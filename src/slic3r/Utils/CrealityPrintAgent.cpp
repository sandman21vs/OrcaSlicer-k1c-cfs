#include "CrealityPrintAgent.hpp"
#include "CrealityPrint.hpp"
#include "Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>

namespace Slic3r {

namespace {

constexpr const char* CrealityPrintAgent_VERSION = "0.1.0";

bool has_visible_base_preset(const PresetCollection& filaments, const std::string& filament_id)
{
    for (const auto& p : filaments.get_presets()) {
        if (p.is_visible && p.is_compatible
            && filaments.get_preset_base(p) == &p
            && p.filament_id == filament_id)
            return true;
    }
    return false;
}

// CFS spool material code -> (vendor, product name, base type). Extracted from
// Creality's on-printer material database
// (/mnt/UDISK/creality/userdata/box/material_database.json; mirrored by the
// community K2-RFID project, db/{k1,k2,hi}.json). The CFS `box` object and the
// MIFARE tag report a filamentId whose last 5 digits are this catalogue id (the
// leading digit is a printer-series prefix). Resolving it to brand + product name
// lets match_filament_preset() pick the exact Orca preset ("Hyper PLA",
// "CR-PLA Matte", ...) instead of only a generic-by-type fallback.
struct CfsCatalogEntry { const char* vendor; const char* name; const char* type; };

const std::map<std::string, CfsCatalogEntry>& cfs_catalog()
{
    static const std::map<std::string, CfsCatalogEntry> tbl = {
        {"00001", {"Generic", "Generic PLA", "PLA"}},
        {"00002", {"Generic", "Generic PLA-Silk", "PLA"}},
        {"00003", {"Generic", "Generic PETG", "PETG"}},
        {"00004", {"Generic", "Generic ABS", "ABS"}},
        {"00005", {"Generic", "Generic TPU", "TPU"}},
        {"00006", {"Generic", "Generic PLA-CF", "PLA-CF"}},
        {"00007", {"Generic", "Generic ASA", "ASA"}},
        {"00008", {"Generic", "Generic PA", "PA"}},
        {"00009", {"Generic", "Generic PA-CF", "PA-CF"}},
        {"00010", {"Generic", "Generic BVOH", "BVOH"}},
        {"00011", {"Generic", "Generic PVA", "PVA"}},
        {"00012", {"Generic", "Generic HIPS", "HIPS"}},
        {"00013", {"Generic", "Generic PET-CF", "PET-CF"}},
        {"00014", {"Generic", "Generic PETG-CF", "PETG-CF"}},
        {"00015", {"Generic", "Generic PA6-CF", "PA6-CF"}},
        {"00016", {"Generic", "Generic PAHT-CF", "PAHT-CF"}},
        {"00017", {"Generic", "Generic PPS", "PPS"}},
        {"00018", {"Generic", "Generic PPS-CF", "PPS-CF"}},
        {"00019", {"Generic", "Generic PP", "PP"}},
        {"00020", {"Generic", "Generic PET", "PET"}},
        {"00021", {"Generic", "Generic PC", "PC"}},
        {"00022", {"Generic", "Generic PA612-CF", "PA-CF"}},
        {"00023", {"Generic", "Generic Support for PA", "PA"}},
        {"00024", {"Generic", "Generic Support for PLA", "PLA"}},
        {"00025", {"Generic", "Generic PA12-CF", "PA-CF"}},
        {"00026", {"Generic", "Generic TPU 64D", "TPU"}},
        {"00027", {"Generic", "Generic PETG-GF", "PETG-GF"}},
        {"00031", {"Generic", "Generic PP-CF", "PP-CF"}},
        {"00032", {"Generic", "Generic PCTG", "PCTG"}},
        {"00033", {"Generic", "Generic ASA-CF", "ASA-CF"}},
        {"00034", {"Generic", "Generic PA6-GF", "PA-GF"}},
        {"00035", {"eSUN", "PLA-LW", "PLA"}},
        {"01001", {"Creality", "Hyper PLA", "PLA"}},
        {"01002", {"Creality", "Hyper L-W PLA", "PLA"}},
        {"01004", {"Creality", "Hyper Stardust", "PLA"}},
        {"01601", {"Creality", "Soleyin Ultra PLA", "PLA"}},
        {"02001", {"Creality", "Hyper PLA-CF", "PLA-CF"}},
        {"03001", {"Creality", "Hyper ABS", "ABS"}},
        {"04001", {"Creality", "CR-PLA", "PLA"}},
        {"05001", {"Creality", "CR-Silk", "PLA"}},
        {"06001", {"Creality", "CR-PETG", "PETG"}},
        {"06002", {"Creality", "Hyper PETG", "PETG"}},
        {"06003", {"Creality", "Hyper PETG-CF", "PETG-CF"}},
        {"07001", {"Creality", "CR-ABS", "ABS"}},
        {"07002", {"Creality", "Hyper PC", "PC"}},
        {"08001", {"Creality", "Ender-PLA", "PLA"}},
        {"09001", {"Creality", "EN-PLA+", "PLA"}},
        {"09002", {"Creality", "ENDER FAST PLA", "PLA"}},
        {"10001", {"Creality", "HP-TPU", "TPU"}},
        {"11001", {"Creality", "CR-Nylon", "PA"}},
        {"12002", {"Creality", "Hyper PPA-CF", "PA-CF"}},
        {"12003", {"Creality", "Hyper PAHT-CF", "PA-CF"}},
        {"12004", {"Creality", "Hyper PA612-CF", "PA612-CF"}},
        {"12005", {"Creality", "Hyper PA6-CF", "PA6-CF"}},
        {"13001", {"Creality", "CR-PLA Carbon", "PLA-CF"}},
        {"14001", {"Creality", "CR-PLA Matte", "PLA"}},
        {"15001", {"Creality", "CR-PLA Fluo", "PLA"}},
        {"16001", {"Creality", "CR-TPU", "TPU"}},
        {"17001", {"Creality", "CR-Wood", "PLA"}},
        {"18001", {"Creality", "HP Ultra PLA", "PLA"}},
        {"19001", {"Creality", "HP-ASA", "ASA"}},
        {"29001", {"Creality", "Hyper Marble", "PLA"}},
        {"E1001", {"eSUN", "PLA+", "PLA"}},
        {"P1001", {"Polymaker", "Panchroma PLA Satin", "PLA"}},
        {"P1002", {"Polymaker", "PolySonic PLA Pro", "PLA"}},
        {"P1003", {"Polymaker", "Panchroma PLA Matte", "PLA"}},
    };
    return tbl;
}

// Resolve a CFS filamentId (5- or 6-digit; last 5 digits are the catalogue id) to
// vendor/name/type. Returns false when the code is not in the catalogue.
bool cfs_lookup_material(const std::string& code, std::string& vendor, std::string& name, std::string& type)
{
    const auto& tbl = cfs_catalog();
    auto try_key = [&](const std::string& k) {
        auto it = tbl.find(k);
        if (it == tbl.end())
            return false;
        vendor = it->second.vendor;
        name   = it->second.name;
        type   = it->second.type;
        return true;
    };
    if (try_key(code))
        return true;
    if (code.size() > 5 && try_key(code.substr(code.size() - 5)))
        return true;
    return false;
}

} // namespace

// Score visible compatible filament presets against the CFS spool metadata and
// return the best-matching filament_id. Scoring:
//   +20  preset name contains brand_name as a substring
//        (e.g. "Hyper PLA" in "Hyper PLA @Creality K2 0.4 nozzle")
//   +10  preset name contains the vendor substring (e.g. "Creality")
//   Tiebreak: prefer the SYSTEM (shipped) preset over user copies. Brand-
//   specific system presets carry their own filament_id; user copies of
//   generic presets inherit a generic filament_id from their parent, so
//   preferring the user copy can collapse a brand-specific match back to
//   "Generic PLA" via the inherited id. Plus: this code targets upstream
//   OrcaSlicer where shipping the user's local tuning would be wrong.
// Requires the preset's declared filament_type to equal the spool's base type
// (PLA/PETG/ABS/...) so we never auto-pick a PETG preset for a PLA spool.
// Falls back to filaments.filament_id_by_type(base_type) when nothing scores.
std::string CrealityPrintAgent::match_filament_preset(const PresetCollection& filaments,
                                                      const std::string&      vendor,
                                                      const std::string&      brand_name,
                                                      const std::string&      base_type)
{
    auto to_lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    const std::string vendor_lower = to_lower(vendor);
    const std::string brand_lower  = to_lower(brand_name);
    const std::string type_lower   = to_lower(base_type);

    struct Match {
        const Preset* preset;
        int           score;
        bool          is_user;
    };
    std::vector<Match> matches;

    int considered = 0;
    for (const auto& p : filaments.get_presets()) {
        if (!p.is_visible || !p.is_compatible) continue;
        // Note: we deliberately do NOT filter on get_preset_base(p) == &p.
        // K2 owners frequently keep tweaked copies of system presets
        // (e.g. "Creality Hyper PLA @K2 (Harky)" with their per-spool PA),
        // which are derived presets — filtering to bases-only would skip
        // exactly the presets users care about most.
        ++considered;

        std::string preset_type;
        if (const auto* ft = p.config.option<ConfigOptionStrings>("filament_type"))
            if (!ft->values.empty()) preset_type = ft->values.front();
        if (to_lower(preset_type) != type_lower) continue;

        const std::string name_lower = to_lower(p.name);
        int score = 0;
        if (!brand_lower.empty() && name_lower.find(brand_lower) != std::string::npos)
            score += 20;
        if (!vendor_lower.empty() && name_lower.find(vendor_lower) != std::string::npos)
            score += 10;

        if (score > 0)
            matches.push_back({&p, score, !p.is_system && !p.is_default});
    }

    if (matches.empty()) {
        const std::string fallback = filaments.filament_id_by_type(base_type);
        const bool        fallback_ok = has_visible_base_preset(filaments, fallback);
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: no preset scored for spool {" << vendor << " "
            << brand_name << " (" << base_type << ")} after considering " << considered
            << " presets; falling back to generic preset id \"" << fallback << "\""
            << (fallback_ok ? "" : " (NOT visible — returning empty)");
        return fallback_ok ? fallback : std::string();
    }

    std::sort(matches.begin(), matches.end(),
              [](const Match& a, const Match& b) {
                  if (a.score   != b.score)   return a.score > b.score;
                  if (a.is_user != b.is_user) return !a.is_user; // prefer system over user
                  return false;
              });

    BOOST_LOG_TRIVIAL(info)
        << "CrealityPrintAgent: matched spool {" << vendor << " " << brand_name
        << " (" << base_type << ")} -> preset \"" << matches.front().preset->name
        << "\" (score=" << matches.front().score
        << ", " << matches.size() << " candidate(s) of " << considered << " considered)";

    return matches.front().preset->filament_id;
}

CrealityPrintAgent::CrealityPrintAgent(std::string log_dir)
    : MoonrakerPrinterAgent(std::move(log_dir))
{
}

AgentInfo CrealityPrintAgent::get_agent_info_static()
{
    return AgentInfo{
        "crealityprint",
        "CrealityPrint",
        CrealityPrintAgent_VERSION,
        "Creality K-series printer agent (CFS-aware filament sync)"
    };
}

std::string CrealityPrintAgent::normalize_filament_type(const std::string& filament_type)
{
    static const std::vector<std::string> bases = {
        "PETG", "PET", "PLA", "ABS", "ASA", "TPU", "PC", "PA", "PVA", "HIPS"
    };
    for (const auto& base : bases) {
        if (filament_type.rfind(base, 0) == 0) return base;
    }
    return filament_type;
}

// Parse the boxsInfo JSON returned by CrealityPrint::query_boxes_info().
// Schema (verified 2026-05-06 against K2 Combo F021 firmware v1.1.260206):
//   { "boxsInfo": { "materialBoxs": [
//     { "id": int, "state": int, "type": int,    // type 0 = CFS, 1 = single-spool external
//       "materials": [
//         { "id": int, "state": int,             // state 1 = loaded
//           "vendor": str, "type": str, "name": str,
//           "color": "#0RRGGBB" }, ...
//       ]}, ...
//   ]}}
bool CrealityPrintAgent::parse_cfs_response(const std::string&    response,
                                            std::vector<CFSSlot>& slots,
                                            int&                  box_count,
                                            std::string&          error)
{
    using nlohmann::json;

    slots.clear();
    box_count = 0;

    if (response.empty()) {
        error = "empty response";
        return false;
    }

    json resp;
    try {
        resp = json::parse(response);
    } catch (const std::exception& e) {
        error = std::string("JSON parse error: ") + e.what();
        return false;
    }

    if (!resp.contains("boxsInfo") || !resp["boxsInfo"].contains("materialBoxs")) {
        error = "invalid schema (missing boxsInfo.materialBoxs)";
        return false;
    }

    // Sequential AMS-style index for accepted CFS boxes. The K2's raw box.id has
    // gaps (id 0 is the external spool holder, type=1, skipped) — using the raw id
    // would publish phantom slots for the gap. Renumber accepted boxes 0,1,2,...
    int cfs_count = 0;
    for (const auto& box : resp["boxsInfo"]["materialBoxs"]) {
        const int box_st   = box.value("state", 0);
        const int box_type = box.value("type",  0);
        if (box_st != 1)   continue; // inactive boxes
        if (box_type != 0) continue; // non-CFS (external spool holder, handled separately by upload dialog)

        const int cfs_index = cfs_count++;

        if (!box.contains("materials") || !box["materials"].is_array())
            continue;

        for (const auto& mat : box["materials"]) {
            // CFS slot state encoding observed across K2 family firmwares:
            //   * K2 (base) / K2 Pro    : 0 = empty, 1 = loaded.
            //   * K2 Plus (1.1.5.5/CFS 1.4.2 onwards): 0 = empty,
            //                                          1 = loaded AND currently
            //                                              selected as the active
            //                                              spool for printing,
            //                                          2 = loaded but not selected.
            // We treat anything non-zero as loaded. Belt-and-braces: also skip
            // entries that look blank (no vendor and no type) regardless of state.
            const int s_state = mat.value("state", 0);
            const std::string s_vendor = mat.value("vendor", std::string());
            const std::string s_type   = mat.value("type",   std::string());
            if (s_state == 0)                       continue; // explicitly empty
            if (s_vendor.empty() && s_type.empty()) continue; // blank entry — likely empty under a different state encoding

            CFSSlot s;
            s.box_id        = cfs_index;
            s.slot_id       = mat.value("id",     0);
            s.vendor        = s_vendor;
            s.brand_name    = mat.value("name",   "");
            s.filament_type = s_type;
            s.color_hex     = mat.value("color",  "#FFFFFF");

            // Creality reports colour as "#0RRGGBB" (8 chars with a leading zero
            // after '#'). Normalise to standard "#RRGGBB".
            if (s.color_hex.size() == 8 && s.color_hex[0] == '#')
                s.color_hex = "#" + s.color_hex.substr(2);

            slots.push_back(std::move(s));
        }
    }

    box_count = cfs_count;
    return true;
}

// Parse the Klipper `box` object (K1-series CFS, e.g. K1C) fetched from Moonraker
// at /printer/objects/query?box. Schema (verified 2026-06-20 against a K1C, CFS
// firmware box version 1.1.3):
//   "box": {
//     "same_material": [
//       ["103001","0000000",["T1A"],"ABS"],   // [type_code, "0RRGGBB", [slot labels], type]
//       ["114001","00B359A",["T1B"],"PLA"], ... ],
//     "T1": { "color_value": ["0RRGGBB",...4], "material_type": [...4],
//             "remain_len": [...4], "state": "connect", ... },
//     "T2".."T4": { ... "state": "None" when no box attached }
//   }
// Slot labels encode position: "T<box><slot>" with box 1..4 and slot A..D.
// Unlike the K2's port-9999 boxsInfo, the K1C exposes the CFS purely through
// Moonraker, so we reuse the base agent's HTTP plumbing.
bool CrealityPrintAgent::fetch_cfs_box_object(std::vector<AmsTrayData>& trays, int& max_slot_index)
{
    trays.clear();
    max_slot_index = 0;

    // Derive the bare host. On the K1C the device IP's port 80 serves Creality's
    // own control API (not Moonraker), so we must target Moonraker's port. We try
    // the canonical direct port (7125) first, then the nginx-proxied web ports
    // (Creality K-series ship Mainsail/Fluidd on 4408/4409), then whatever
    // base_url the agent recorded. Verified 2026-06-20: K1C answers on :7125 and
    // :4409 but 404s on :80.
    std::string host = device_info.dev_ip;
    if (host.empty() && !device_info.base_url.empty()) {
        host = device_info.base_url;
        if (auto p = host.find("://"); p != std::string::npos)
            host = host.substr(p + 3);
        if (auto s = host.find('/'); s != std::string::npos)
            host = host.substr(0, s);
        if (auto c = host.rfind(':'); c != std::string::npos)
            host = host.substr(0, c);
    }
    if (host.empty())
        return false;

    std::vector<std::string> candidates = {
        "http://" + host + ":7125",
        "http://" + host + ":4409",
        "http://" + host + ":4408",
    };
    if (!device_info.base_url.empty())
        candidates.push_back(device_info.base_url);

    auto query_box = [&](const std::string& base) -> std::string {
        std::string body;
        bool        ok   = false;
        auto        http = Http::get(join_url(base, "/printer/objects/query?box"));
        if (!device_info.api_key.empty())
            http.header("X-Api-Key", device_info.api_key);
        http.timeout_connect(3)
            .timeout_max(8)
            .on_complete([&](std::string b, unsigned status) { if (status == 200) { body = std::move(b); ok = true; } })
            .on_error([&](std::string, std::string, unsigned) {})
            .perform_sync();
        return ok ? body : std::string();
    };

    nlohmann::json json;
    bool           found = false;
    for (const auto& base : candidates) {
        std::string body = query_box(base);
        if (body.empty())
            continue;
        json = nlohmann::json::parse(body, nullptr, false, true);
        if (json.is_discarded())
            continue;
        if (json.contains("result") && json["result"].contains("status")
            && json["result"]["status"].contains("box")) {
            BOOST_LOG_TRIVIAL(info) << "CrealityPrintAgent: found Moonraker `box` object at " << base;
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    const auto& box = json["result"]["status"]["box"];
    if (!box.contains("same_material") || !box["same_material"].is_array())
        return false;

    auto* bundle = GUI::wxGetApp().preset_bundle;

    for (const auto& entry : box["same_material"]) {
        if (!entry.is_array() || entry.size() < 4)
            continue;
        const std::string code      = entry[0].is_string() ? entry[0].get<std::string>() : std::string();
        const std::string color_raw = entry[1].is_string() ? entry[1].get<std::string>() : std::string();
        const auto&       labels    = entry[2];
        const std::string type_str  = entry[3].is_string() ? entry[3].get<std::string>() : std::string();
        if (type_str.empty() || !labels.is_array())
            continue;

        // Resolve the filamentId code to vendor + product name via the catalogue,
        // so we can match the exact Orca preset. Fall back to the box's own type
        // string when the code is unknown.
        std::string cat_vendor, cat_name, cat_type;
        const bool  resolved  = cfs_lookup_material(code, cat_vendor, cat_name, cat_type);
        const std::string base_type =
            normalize_filament_type(resolved && !cat_type.empty() ? cat_type : type_str);

        // Color arrives as "0RRGGBB" (7 hex chars, leading 0). Keep only hex
        // digits and take the last 6 so build_ams_payload's normalizer accepts it.
        std::string color_hex;
        for (char c : color_raw)
            if (std::isxdigit(static_cast<unsigned char>(c)))
                color_hex.push_back(c);
        if (color_hex.size() > 6)
            color_hex = color_hex.substr(color_hex.size() - 6);

        // match_filament_preset scores by brand/vendor substrings and falls back
        // to filament_id_by_type(base_type) when both are empty (unknown code).
        std::string info_idx = bundle
            ? match_filament_preset(bundle->filaments, cat_vendor, cat_name, base_type)
            : map_filament_type_to_generic_id(base_type);

        for (const auto& lbl_json : labels) {
            if (!lbl_json.is_string())
                continue;
            const std::string lbl = lbl_json.get<std::string>(); // e.g. "T1A"
            if (lbl.size() < 3 || std::toupper(static_cast<unsigned char>(lbl[0])) != 'T')
                continue;

            const char slot_ch = static_cast<char>(std::toupper(static_cast<unsigned char>(lbl.back())));
            if (slot_ch < 'A' || slot_ch > 'Z')
                continue;
            const int slot = slot_ch - 'A';

            int box_num = 0;
            try {
                box_num = std::stoi(lbl.substr(1, lbl.size() - 2));
            } catch (...) {
                continue;
            }
            if (box_num < 1)
                continue;

            const int slot_index = (box_num - 1) * 4 + slot;

            AmsTrayData tray;
            tray.slot_index    = slot_index;
            tray.has_filament  = true;
            tray.tray_type     = base_type;
            tray.tray_color    = color_hex;
            tray.tray_info_idx = info_idx;
            trays.push_back(std::move(tray));

            max_slot_index = std::max(max_slot_index, slot_index);
        }
    }

    return !trays.empty();
}

bool CrealityPrintAgent::fetch_filament_info(std::string dev_id)
{
    // 1) K1-series CFS (e.g. K1C) is exposed as the Klipper `box` object via
    //    Moonraker — no proprietary port-9999 WebSocket. Try this first.
    {
        std::vector<AmsTrayData> trays;
        int                      max_slot_index = 0;
        if (fetch_cfs_box_object(trays, max_slot_index)) {
            const int ams_count = (max_slot_index + 4) / 4;
            BOOST_LOG_TRIVIAL(info)
                << "CrealityPrintAgent: CFS (Klipper box) with " << trays.size()
                << " loaded slot(s) across " << ams_count << " box(es)";
            build_ams_payload(ams_count, max_slot_index, trays);
            return true;
        }
    }

    // 2) K2-platform CFS is exposed via the proprietary port-9999 WebSocket
    //    boxsInfo. Requires the device IP.
    if (device_info.dev_ip.empty()) {
        BOOST_LOG_TRIVIAL(warning)
            << "CrealityPrintAgent::fetch_filament_info: no device IP, falling back to base agent";
        return MoonrakerPrinterAgent::fetch_filament_info(std::move(dev_id));
    }

    // Build a CrealityPrint helper so we can use its model detection + WS helpers
    // (added in upstream PR #13291).
    DynamicPrintConfig cfg;
    cfg.set_key_value("print_host",                  new ConfigOptionString("http://" + device_info.dev_ip));
    cfg.set_key_value("print_host_webui",            new ConfigOptionString(""));
    cfg.set_key_value("printhost_cafile",            new ConfigOptionString(""));
    cfg.set_key_value("printhost_port",              new ConfigOptionString(""));
    cfg.set_key_value("printhost_apikey",            new ConfigOptionString(device_info.api_key));
    cfg.set_key_value("printhost_ssl_ignore_revoke", new ConfigOptionBool(false));

    CrealityPrint host(&cfg);

    // Dynamic CFS detection: don't gate on a hard-coded model allowlist (which
    // excluded the K1C). Always attempt the CFS query; the parse + box_count
    // checks below decide whether this printer actually has a CFS. Non-CFS
    // boards (or an unreachable query) fall back to the base Moonraker agent.
    BOOST_LOG_TRIVIAL(info)
        << "CrealityPrintAgent: probing CFS slots on " << host.model_name();

    const std::string response = host.query_boxes_info();

    std::vector<CFSSlot> slots;
    int                  box_count = 0;
    std::string          parse_err;
    if (!parse_cfs_response(response, slots, box_count, parse_err)) {
        BOOST_LOG_TRIVIAL(warning)
            << "CrealityPrintAgent: CFS query failed (" << parse_err << "), "
            << "falling back to base agent";
        return MoonrakerPrinterAgent::fetch_filament_info(std::move(dev_id));
    }

    if (box_count == 0) {
        // No active CFS boxes attached — printer is in direct-spool mode. Let the
        // base agent take over so the user still gets whatever filament info
        // Moonraker exposes.
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: no active CFS boxes, deferring to base agent";
        return MoonrakerPrinterAgent::fetch_filament_info(std::move(dev_id));
    }

    BOOST_LOG_TRIVIAL(info)
        << "CrealityPrintAgent: " << box_count << " CFS box(es), "
        << slots.size() << " loaded slot(s)";

    // Index loaded slots by (box, slot) for O(1) lookup as we walk the full
    // box_count * 4 grid, emitting an AmsTrayData entry for each physical slot.
    std::map<std::pair<int, int>, const CFSSlot*> by_position;
    for (const auto& s : slots)
        by_position[{s.box_id, s.slot_id}] = &s;

    auto* bundle = GUI::wxGetApp().preset_bundle;

    const int max_slots = box_count * 4;
    std::vector<AmsTrayData> trays;
    trays.reserve(max_slots);

    for (int box = 0; box < box_count; ++box) {
        for (int idx = 0; idx < 4; ++idx) {
            AmsTrayData tray;
            tray.slot_index = box * 4 + idx;

            auto it = by_position.find({box, idx});
            if (it == by_position.end()) {
                tray.has_filament = false;
                trays.push_back(std::move(tray));
                continue;
            }

            const CFSSlot& s = *it->second;
            tray.has_filament = true;
            tray.tray_type    = normalize_filament_type(s.filament_type);
            tray.tray_color   = s.color_hex;

            if (bundle) {
                tray.tray_info_idx = match_filament_preset(
                    bundle->filaments, s.vendor, s.brand_name, tray.tray_type);
            }

            trays.push_back(std::move(tray));
        }
    }

    build_ams_payload(box_count, max_slots - 1, trays);
    return true;
}

} // namespace Slic3r

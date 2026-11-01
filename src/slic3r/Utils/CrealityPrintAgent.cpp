#include "CrealityPrintAgent.hpp"
#include "CrealityHostDiscovery.hpp"
#include "CrealityPrint.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "bambu_networking.hpp"
#include "IPrinterAgent.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaAmsSetting.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>
#include <wx/string.h>
#include "libslic3r/Config.hpp"
#include <exception>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <utility>

using json = nlohmann::json;

namespace Slic3r {

namespace {

constexpr const char* CrealityPrintAgent_VERSION = "0.1.0";

// A CrealityPrint helper for its model detection and WebSocket commands (upstream PR #13291).
// The slot index, four per box, of a box object's slot name such as "T1C", or -1 for anything else.
int box_slot_index(const json& name)
{
    const std::string text = name.is_string() ? name.get<std::string>() : "";
    if (text.size() != 3 || text[0] != 'T' || text[1] < '1' || text[1] > '4' || text[2] < 'A' || text[2] > 'D')
        return -1;
    return (text[1] - '1') * 4 + (text[2] - 'A');
}

CrealityPrint make_host(const std::string& dev_ip, const std::string& api_key)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("print_host",                  new ConfigOptionString("http://" + dev_ip));
    cfg.set_key_value("print_host_webui",            new ConfigOptionString(""));
    cfg.set_key_value("printhost_cafile",            new ConfigOptionString(""));
    cfg.set_key_value("printhost_port",              new ConfigOptionString(""));
    cfg.set_key_value("printhost_apikey",            new ConfigOptionString(api_key));
    cfg.set_key_value("printhost_ssl_ignore_revoke", new ConfigOptionBool(false));
    return CrealityPrint(&cfg);
}

std::string to_lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

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

// Lower-case words of a preset name with the "@scope" suffix dropped:
// "Generic PLA Matte @Creality K2-all" -> {"generic", "pla", "matte"}.
std::vector<std::string> name_words(const std::string& name)
{
    std::vector<std::string> words;
    std::string              word;
    for (char c : name.substr(0, name.find('@'))) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    }
    if (!word.empty())
        words.push_back(word);
    return words;
}

std::string json_string(const json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Numbers arrive as JSON numbers or as numeric strings, depending on the command and firmware.
double json_number(const json& obj, const char* key, double fallback = 0.)
{
    const auto it = obj.find(key);
    if (it == obj.end())
        return fallback;
    if (it->is_number())
        return it->get<double>();
    if (it->is_string()) {
        const auto& text  = it->get_ref<const std::string&>();
        char*       end   = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        return end != text.c_str() ? value : fallback;
    }
    return fallback;
}

// "RRGGBBAA" (Orca's AMS spelling), "#RRGGBB" or "#0RRGGBB" (Creality's) -> lower-case "rrggbb",
// or "" when unreadable.
std::string color_rgb(std::string color)
{
    if (!color.empty() && color[0] == '#')
        color.erase(0, 1);
    if (color.size() == 7 && color[0] == '0')
        color.erase(0, 1);
    if (color.size() < 6)
        return {};
    color.resize(6);
    for (auto& c : color) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return {};
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return color;
}

bool is_ams_filament_setting(const json& msg)
{
    return msg.is_object() && msg.contains("print") && msg["print"].is_object()
        && json_string(msg["print"], "command") == "ams_filament_setting";
}

// The filament preset a Device tab slot edit refers to, by its filament_id.
const Preset* find_filament_preset(const PresetCollection& filaments, const std::string& filament_id)
{
    const Preset* found = nullptr;
    for (const auto& p : filaments.get_presets()) {
        if (p.filament_id != filament_id || p.is_default)
            continue;
        if (p.is_visible && p.is_compatible)
            return &p;
        if (found == nullptr)
            found = &p;
    }
    return found;
}

} // namespace

// Score visible compatible filament presets against the CFS spool metadata and
// return the best-matching filament_id. Scoring:
//   +20  preset name contains brand_name as a substring
//        (e.g. "Hyper PLA" in "Hyper PLA @Creality K2 0.4 nozzle")
//   +10  the preset belongs to the spool's vendor - by its owning VendorProfile OR by
//        its name. The profile test is what finds the vendor's own generics, which are
//        named "Generic <material> @<scope>" and do not repeat the vendor; the name test
//        still finds a third party filament shipped inside that vendor's bundle, which
//        carries the bundle owner's profile but names its real brand.
//    -5  per word of the preset name the spool never mentioned ("generic" excepted -
//        it marks the unbranded base product rather than a qualifier), so the least
//        specific preset that still explains the spool wins. Without it a spool
//        reporting only "PLA" scores "Generic PLA High Speed" and "Generic PLA Matte"
//        exactly as high as "Generic PLA"; those are three products with three
//        filament_ids, so whichever sorted first won and the printer got the wrong
//        one. Applied after the score gate, so it only reorders genuine matches.
//   Tiebreak: prefer the SYSTEM (shipped) preset over user copies, then by name so the
//   winner never depends on how std::sort leaves equal elements. User copies of generic
//   presets inherit a generic filament_id from their parent, so preferring the user copy
//   can collapse a brand-specific match back to "Generic PLA" via the inherited id.
// Requires the preset's declared filament_type to equal the spool's base type
// (PLA/PETG/ABS/...) so we never auto-pick a PETG preset for a PLA spool.
// Falls back to a visible system base of the same type when nothing scores, then to a system
// preset that is not enabled.
std::string CrealityPrintAgent::match_filament_preset(const PresetCollection& filaments,
                                                      const std::string&      vendor,
                                                      const std::string&      brand_name,
                                                      const std::string&      base_type)
{
    if (base_type.empty()) return {}; // a spool the printer could not name has no preset

    const std::string vendor_lower = to_lower(vendor);
    const std::string brand_lower  = to_lower(brand_name);
    const std::string type_lower   = to_lower(base_type);

    // Everything the spool told us about itself, as words. A word in a preset's name
    // that is not in here is a qualifier the spool never claimed.
    std::set<std::string> spool_words{"generic"};
    for (const std::string& src : {brand_lower, vendor_lower, type_lower})
        for (auto& w : name_words(src))
            spool_words.insert(std::move(w));

    struct Match {
        const Preset* preset;
        int           score;
        bool          is_user;
    };
    std::vector<Match> matches;

    int        considered    = 0;
    const auto score_presets = [&](bool visible) {
        for (const auto& p : filaments.get_presets()) {
            if (p.is_visible != visible || !p.is_compatible) continue;
            if (!visible && !p.is_system) continue;
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
            // Profile OR name - neither alone covers both the vendor's own generics and the
            // third party filaments shipped inside its bundle. See the header comment.
            if (!vendor_lower.empty()
                && ((p.vendor != nullptr && to_lower(p.vendor->name) == vendor_lower)
                    || name_lower.find(vendor_lower) != std::string::npos))
                score += 10;

            if (score == 0) continue;

            for (const auto& w : name_words(p.name))
                if (spool_words.count(w) == 0)
                    score -= 5;

            matches.push_back({&p, score, !p.is_system && !p.is_default});
        }
    };
    score_presets(/*visible=*/true);

    if (matches.empty()) {
        // first_visible_idx_by_type() returns any visible preset when no base of this
        // type exists. Never sync an unrelated material to the CFS.
        const auto&       fallback_preset = filaments.preset(filaments.first_visible_idx_by_type(base_type));
        const std::string fallback        = fallback_preset.filament_id;
        const bool        fallback_ok     = fallback_preset.config.opt_string("filament_type", 0u) == base_type &&
                                    has_visible_base_preset(filaments, fallback);
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: no preset scored for spool {" << vendor << " "
            << brand_name << " (" << base_type << ")} after considering " << considered
            << " presets; falling back to generic preset id \"" << fallback << "\""
            << (fallback_ok ? "" : " (no visible base of this type)");
        if (fallback_ok)
            return fallback;

        // The spool's own preset may ship with the printer without being enabled, and the AMS
        // sync finds a preset by its filament_id whether it is enabled or not.
        score_presets(/*visible=*/false);
        if (matches.empty())
            return {};
    }

    std::sort(matches.begin(), matches.end(),
              [](const Match& a, const Match& b) {
                  if (a.score   != b.score)   return a.score > b.score;
                  if (a.is_user != b.is_user) return !a.is_user; // prefer system over user
                  return a.preset->name < b.preset->name;        // keep the winner deterministic
              });

    BOOST_LOG_TRIVIAL(info)
        << "CrealityPrintAgent: matched spool {" << vendor << " " << brand_name
        << " (" << base_type << ")} -> preset \"" << matches.front().preset->name
        << "\" (score=" << matches.front().score
        << ", " << matches.size() << " candidate(s) of " << considered << " considered)";

    return matches.front().preset->filament_id;
}

CrealityPrintAgent::~CrealityPrintAgent()
{
    {
        std::lock_guard<std::mutex> lock(watch_mutex);
        watch_stop = true;
    }
    watch_wake.notify_all();
    if (watch_thread.joinable())
        watch_thread.join();
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
    if (boost::iends_with(filament_type, "-CF") || boost::iends_with(filament_type, "-GF"))
        return filament_type;

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
//           "vendor": str, "type": str, "name": str, // all "" for an unknown RFID tag
//           "color": "#dRRGGBB" }, ...
//       ]}, ...
//   ]}}
bool CrealityPrintAgent::parse_cfs_response(const std::string&    response,
                                            std::vector<CFSSlot>& slots,
                                            int&                  box_count,
                                            std::string&          error)
{

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
        const int box_st   = safe_json_int(box, "state");
        const int box_type = safe_json_int(box, "type");
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
            // We treat anything non-zero as loaded. A loaded spool with an unknown RFID
            // tag reports only its colour, so only an entry with no colour either is blank.
            const int s_state = safe_json_int(mat, "state");
            const std::string s_vendor = safe_json_string(mat, "vendor");
            const std::string s_type   = safe_json_string(mat, "type");
            const std::string s_color  = safe_json_string(mat, "color");
            if (s_state == 0)                                          continue; // explicitly empty
            if (s_vendor.empty() && s_type.empty() && s_color.empty()) continue; // blank entry

            CFSSlot s;
            s.box_id        = cfs_index;
            s.printer_box_id = safe_json_int(box, "id");
            s.slot_id       = safe_json_int(mat, "id");
            s.vendor        = s_vendor;
            s.brand_name    = safe_json_string(mat, "name");
            s.filament_type = s_type;
            s.color_hex     = s_color.empty() ? "#FFFFFF" : s_color;

            // Percentage left on the spool, as the CFS measures it. Boxes that do not
            // report it leave the slot at -1 so the UI keeps showing an unknown level
            // instead of an empty spool.
            if (const auto percent = mat.find("percent"); percent != mat.end() && percent->is_number())
                s.remain = percent->get<int>();

            // Creality reports colour as "#" + a digit + "RRGGBB". Drop the digit,
            // as Creality Print does, to get a standard "#RRGGBB".
            if (s.color_hex.size() == 8 && s.color_hex[0] == '#')
                s.color_hex = "#" + s.color_hex.substr(2);

            slots.push_back(std::move(s));
        }
    }

    box_count = cfs_count;
    return true;
}

// Each connected box reports under its own "T<n>" key which of its slots feeds the toolhead,
// as the slot's letter, or "None":
//   { "result": { "status": { "box": { "T1": { "state": "connect", "filament": "A" } } } } }
std::optional<int> CrealityPrintAgent::parse_loaded_slot(const std::string& response)
{
    const json resp = json::parse(response, nullptr, false);
    if (resp.is_discarded() || !resp.contains("result") || !resp["result"].contains("status")
        || !resp["result"]["status"].contains("box"))
        return std::nullopt;

    const auto&        box = resp["result"]["status"]["box"];
    std::optional<int> loaded;
    for (int box_num = 1; box_num <= 4; ++box_num) {
        const std::string key = "T" + std::to_string(box_num);
        if (!box.contains(key) || !box[key].is_object() || box[key].value("state", "") != "connect")
            continue;
        loaded = loaded.value_or(-1);
        const std::string filament = box[key].value("filament", "");
        if (filament.size() == 1 && filament[0] >= 'A' && filament[0] <= 'D')
            return (box_num - 1) * 4 + (filament[0] - 'A');
    }
    return loaded;
}

void CrealityPrintAgent::add_slots_from_box_object(const std::string& box_response, std::vector<CFSSlot>& slots)
{
    const json resp = json::parse(box_response, nullptr, false);
    if (resp.is_discarded() || !resp.contains("result") || !resp["result"].contains("status") || !resp["result"]["status"].contains("box"))
        return;

    const json& box = resp["result"]["status"]["box"];
    if (!box.contains("same_material") || !box["same_material"].is_array())
        return;

    for (const json& material : box["same_material"]) {
        if (!material.is_array() || material.size() < 4 || !material[1].is_string() || !material[2].is_array() || !material[3].is_string())
            continue;

        for (const json& name : material[2]) {
            const int index = box_slot_index(name);
            if (index < 0 || std::any_of(slots.begin(), slots.end(), [index](const CFSSlot& s) { return s.box_id * 4 + s.slot_id == index; }))
                continue;

            // The box writes colours as a digit and RRGGBB, like port 9999 without its "#".
            const std::string color = material[1].get<std::string>();
            CFSSlot           slot;
            slot.box_id         = index / 4;
            slot.printer_box_id = slot.box_id + 1;
            slot.slot_id        = index % 4;
            slot.filament_type  = material[3].get<std::string>();
            slot.material_code  = material[0].is_string() ? material[0].get<std::string>() : std::string();
            slot.color_hex      = "#" + (color.size() == 7 ? color.substr(1) : color);
            slots.push_back(std::move(slot));
        }
    }
}

void CrealityPrintAgent::name_slots_from_library(const std::string& library_response, std::vector<CFSSlot>& slots)
{
    const json resp = json::parse(library_response, nullptr, false);
    if (resp.is_discarded() || !resp.contains("retMaterials") || !resp["retMaterials"].is_array())
        return;

    for (CFSSlot& slot : slots) {
        if (!slot.vendor.empty() || slot.material_code.size() != 6)
            continue;
        const std::string id = slot.material_code.substr(1);
        for (const auto& entry : resp["retMaterials"]) {
            if (!entry.is_object() || !entry.contains("base") || !entry["base"].is_object() || json_string(entry["base"], "id") != id)
                continue;
            slot.vendor     = json_string(entry["base"], "brand");
            slot.brand_name = json_string(entry["base"], "name");
            break;
        }
    }
}

std::vector<int> CrealityPrintAgent::refill_groups(const json& same_material)
{
    std::vector<int> groups;
    if (!same_material.is_array())
        return groups;

    for (const json& material : same_material) {
        if (!material.is_array() || material.size() < 3 || !material[2].is_array())
            continue;

        int group = 0;
        for (const json& slot : material[2]) {
            if (const int index = box_slot_index(slot); index >= 0)
                group |= 1 << index;
        }
        if (group != 0)
            groups.push_back(group);
    }
    return groups;
}

bool CrealityPrintAgent::box_update_changes_slots(const json& box_update, std::map<std::string, std::string>& seen)
{
    if (!box_update.is_object())
        return false;

    bool changed = false;
    auto note    = [&](const std::string& key, const std::string& value) {
        auto it = seen.find(key);
        if (it == seen.end()) {
            seen.emplace(key, value);
        } else if (it->second != value) {
            it->second = value;
            changed    = true;
        }
    };

    // The box also streams its temperature, humidity and feed wheel, which change all the time and
    // leave the slots as they are. The humidity shown beside them is read again with each new level.
    if (box_update.contains("same_material"))
        note("same_material", box_update["same_material"].dump());
    for (int box_num = 1; box_num <= 4; ++box_num) {
        const std::string key = "T" + std::to_string(box_num);
        if (!box_update.contains(key) || !box_update[key].is_object())
            continue;
        const json& box = box_update[key];
        if (box.contains("filament"))
            note(key + ".filament", box["filament"].dump());
        if (const double humidity = json_number(box, "dry_and_humidity"); humidity > 0.)
            note(key + ".humidity_level", std::to_string(humidity_level(humidity)));
    }
    return changed;
}

void CrealityPrintAgent::add_status_objects(const std::set<std::string>& available, std::set<std::string>& subscribe) const
{
    if (available.count("box") != 0)
        subscribe.insert("box");
}

void CrealityPrintAgent::on_status_update(const std::string& dev_id, const json& updates)
{
    // A load, unload or tool change from the printer's own screen or a print moves the filament
    // without this app asking, so the slots are read again whenever the box reports it.
    if (updates.contains("box") && box_update_changes_slots(updates["box"], seen_box_fields))
        fetch_filament_info_on_main(dev_id);

    // Printing again means the printer got past whatever it complained about.
    if (updates.contains("print_stats") && updates["print_stats"].is_object() && updates["print_stats"].value("state", "") == "printing")
        report_printer_message({});
}

void CrealityPrintAgent::add_print_status(const json& status, json& print) const
{
    if (can_command_box && status.contains("box"))
        add_refill_status(status["box"], print);
}

void CrealityPrintAgent::add_refill_status(const json& box, json& print)
{
    if (!box.is_object() || !box.contains("auto_refill"))
        return;

    print["support_filament_backup"] = true;
    if (box["auto_refill"].is_number() && box["auto_refill"].get<int>() != 0)
        print["home_flag"] = print["home_flag"].get<int>() | (1 << 10);
    if (box.contains("same_material"))
        print["filam_bak"] = refill_groups(box["same_material"]);
}

std::optional<std::map<std::string, std::string>> CrealityPrintAgent::color_match_info(const std::string&          ams_mapping_info,
                                                                                        const std::vector<CFSSlot>& slots)
{
    std::map<std::string, std::string> info;
    const json                         mapping = json::parse(ams_mapping_info, nullptr, false);
    if (!mapping.is_array())
        return info;

    for (std::size_t filament = 0; filament < mapping.size(); ++filament) {
        const json& item = mapping[filament];
        const int   tray = item.is_object() && item.contains("ams") && item["ams"].is_number_integer() ? item["ams"].get<int>() : -1;
        if (devPrinterUtil::IsVirtualSlot(tray))
            return std::map<std::string, std::string>{};
        if (tray < 0)
            continue; // a filament the plate does not use

        const auto slot = std::find_if(slots.begin(), slots.end(), [tray](const CFSSlot& s) { return s.box_id * 4 + s.slot_id == tray; });
        if (slot == slots.end())
            return std::nullopt;

        // The firmware matches by G-code tool: T1A for the first filament, T1B for the second,
        // whichever slot it is mapped onto. The keys count only mapped filaments, because
        // start_print_commands() stops at the first one missing.
        const std::string tool = "T1" + std::string(1, char('A' + filament));
        info["colorMatch_" + std::to_string(info.size())] = tool + "\t" + slot->filament_type + "\t" + slot->color_hex + "\t" +
                                                            std::to_string(slot->printer_box_id) + "\t" + std::to_string(slot->slot_id);
    }
    return info;
}

bool CrealityPrintAgent::print_uploaded_file(const PrintParams& params, const std::string& filename)
{
    CrealityPrint host = make_host(device_info.dev_ip, device_info.api_key);
    if (!params.task_use_ams || !host.supports_multi_color_print())
        return MoonrakerPrinterAgent::print_uploaded_file(params, filename);

    // A failed query leaves no slots, so a filament mapped onto one is reported below.
    std::vector<CFSSlot> slots;
    int                  box_count = 0;
    std::string          error;
    if (!parse_cfs_response(host.query_boxes_info(), slots, box_count, error))
        BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: CFS query failed before printing: " << error;
    if (can_command_box)
        add_slots_from_box_object(query_box_object(), slots);

    const auto info = color_match_info(params.ams_mapping_info, slots);
    if (!info) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrintAgent: a filament is mapped onto a slot the CFS does not report: " << params.ams_mapping_info;
        return false;
    }
    if (info->empty())
        return MoonrakerPrinterAgent::print_uploaded_file(params, filename);

    wxString message;
    if (!host.start_print(message, filename, *info)) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrintAgent: the printer did not start the print: " << message.ToUTF8().data();
        return false;
    }
    return true;
}

json CrealityPrintAgent::box_config_fields(const json& print_cmd)
{
    json fields = json::object();
    if (!print_cmd.is_object())
        return fields;

    const std::string command = json_string(print_cmd, "command");
    const auto        copy    = [&](const char* from, const char* to) {
        if (const auto it = print_cmd.find(from); it != print_cmd.end() && it->is_boolean())
            fields[to] = it->get<bool>() ? 1 : 0;
    };
    if (command == "print_option") {
        copy("auto_switch_filament", "autoRefill");
    } else if (command == "ams_user_setting") {
        copy("tray_read_option", "cAutoFeed");
        copy("startup_read_option", "cSelfTest");
    }
    return fields;
}

std::string CrealityPrintAgent::printer_error_text(const std::string& line)
{
    if (line.rfind("!!", 0) != 0)
        return {};

    // The firmware's JSON here is not always valid ({"code":"key61, "msg":...), so it is read by pattern.
    static const std::regex code_pattern(R"re("code"\s*:\s*"(key\d+))re");
    static const std::regex msg_pattern(R"re("msg"\s*:\s*"([^"]*)")re");
    std::smatch             code;
    std::smatch             msg;
    const bool              has_code = std::regex_search(line, code, code_pattern);
    if (has_code && code[1] == "key61")
        return {};
    if (has_code && std::regex_search(line, msg, msg_pattern))
        return code[1].str() + ": " + msg[1].str();

    const size_t start = line.find_first_not_of("! ");
    return start == std::string::npos ? std::string() : line.substr(start);
}

void CrealityPrintAgent::on_gcode_response(const std::string& /*dev_id*/, const std::string& line)
{
    if (std::string message = printer_error_text(line); !message.empty())
        report_printer_message(std::move(message));
}

void CrealityPrintAgent::report_printer_message(std::string message)
{
    run_on_main([this, message = std::move(message)] {
        auto*          dev_manager = GUI::wxGetApp().getDeviceManager();
        MachineObject* obj         = dev_manager ? dev_manager->get_my_machine(device_info.dev_id) : nullptr;
        if (obj)
            obj->printer_message = message;
    });
}

int CrealityPrintAgent::printer_notice(const std::string& response)
{
    const json status = json::parse(response, nullptr, false);
    if (!status.is_object() || !status.contains("err") || !status["err"].is_object())
        return -1;
    return static_cast<int>(json_number(status["err"], "key"));
}

void CrealityPrintAgent::report_printer_notice(int key, int previous_key)
{
    run_on_main([this, key, previous_key] {
        // TRN The printer's firmware numbers the notices on its touch screen. %1% is that number.
        const auto text = [](int notice) { return GUI::format(_u8L("The printer shows notice %1% on its screen."), notice); };

        auto*          dev_manager = GUI::wxGetApp().getDeviceManager();
        MachineObject* obj         = dev_manager ? dev_manager->get_my_machine(device_info.dev_id) : nullptr;
        if (!obj)
            return;
        if (key != 0)
            obj->printer_message = text(key);
        else if (previous_key != 0 && obj->printer_message == text(previous_key))
            obj->printer_message.clear();
    });
}

void CrealityPrintAgent::watch_printers()
{
    constexpr auto notice_interval = std::chrono::seconds(20);
    constexpr auto scan_interval   = std::chrono::minutes(1);
    constexpr int  scan_wait       = 2; // Seconds, short since the destructor waits for the scan

    int                                   shown_key = 0;
    std::chrono::steady_clock::time_point next_scan;
    std::unique_lock<std::mutex>          lock(watch_mutex);
    while (!watch_stop) {
        if (scan_wanted || (discovering && std::chrono::steady_clock::now() >= next_scan)) {
            scan_wanted = false;
            next_scan   = std::chrono::steady_clock::now() + scan_interval;
            lock.unlock();
            add_found_printers(CrealityHostDiscovery::scan(/*probe_info=*/true, scan_wait));
            lock.lock();
            continue;
        }

        watch_wake.wait_for(lock, notice_interval, [this] { return watch_stop || scan_wanted; });
        if (watch_stop || scan_wanted || notice_ip.empty() || !can_command_box)
            continue;

        const CrealityPrint host = make_host(notice_ip, notice_api_key);
        lock.unlock();
        const int key = printer_notice(host.query_printer_status());
        if (key >= 0 && key != shown_key) {
            report_printer_notice(key, shown_key);
            shown_key = key;
        }
        lock.lock();
    }
}

void CrealityPrintAgent::add_found_printers(std::vector<CrealityHost> hosts)
{
    run_on_main([hosts = std::move(hosts)] {
        auto* dev_manager = GUI::wxGetApp().getDeviceManager();
        if (!dev_manager)
            return;

        const auto known = dev_manager->get_local_machinelist();
        for (const CrealityHost& host : hosts) {
            const bool listed = std::any_of(known.begin(), known.end(), [&host](const auto& entry) {
                // An address may name Moonraker's port
                return entry.second && entry.second->get_dev_ip().substr(0, entry.second->get_dev_ip().find(':')) == host.ip;
            });
            if (!host.cfs_capable || listed)
                continue;

            BBLocalMachine machine;
            machine.dev_id       = host.ip;
            machine.dev_ip       = host.ip;
            machine.dev_name     = host.hostname;
            machine.printer_type = "Creality " + host.model_name;
            // These printers take no access code. GUI_App::select_machine() fills in the same one.
            dev_manager->insert_local_device(machine, "lan", "free", "", "88888888");
            BOOST_LOG_TRIVIAL(info) << "CrealityPrintAgent: found " << host.hostname << " at " << host.ip;
        }
    });
}

std::string CrealityPrintAgent::moonraker_base_url(const std::string& base_url)
{
    const size_t scheme = base_url.find("://");
    if (base_url.find(':', scheme == std::string::npos ? 0 : scheme + 3) != std::string::npos)
        return {};

    std::string url = base_url;
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    return url + ":" + CrealityPrint::moonraker_port;
}

std::string CrealityPrintAgent::alternative_base_url(const std::string& base_url) const
{
    return moonraker_base_url(base_url);
}

int CrealityPrintAgent::humidity_level(double percent)
{
    return percent < 20. ? 5 : percent < 40. ? 4 : percent < 60. ? 3 : percent < 80. ? 2 : 1;
}

std::vector<json> CrealityPrintAgent::box_climates(const std::string& response)
{
    std::vector<json> climates;
    const json        reply = json::parse(response, nullptr, false);
    if (!reply.is_object() || !reply.contains("boxsInfo") || !reply["boxsInfo"].contains("materialBoxs"))
        return climates;

    for (const json& box : reply["boxsInfo"]["materialBoxs"]) {
        if (safe_json_int(box, "state") != 1 || safe_json_int(box, "type") != 0)
            continue; // what parse_cfs_response() does not count as a CFS

        json climate = json::object();
        if (const double humidity = json_number(box, "humidity"); humidity > 0.) {
            climate["humidity"]     = std::to_string(humidity_level(humidity));
            climate["humidity_raw"] = std::to_string(std::lround(humidity));
        }
        if (box.contains("temp"))
            climate["temp"] = std::to_string(json_number(box, "temp"));
        climates.push_back(std::move(climate));
    }
    return climates;
}

void CrealityPrintAgent::add_ams_unit_status(int ams_id, json& ams_unit) const
{
    if (ams_id >= 0 && ams_id < static_cast<int>(box_climate.size()))
        ams_unit.update(box_climate[ams_id]);
}

void CrealityPrintAgent::report_box_config(const std::string& response)
{
    auto*          dev_manager = GUI::wxGetApp().getDeviceManager();
    MachineObject* obj         = dev_manager ? dev_manager->get_my_machine(device_info.dev_id) : nullptr;
    const json     reply       = json::parse(response, nullptr, false);
    if (!obj || reply.is_discarded() || !reply.contains("boxConfig") || !reply["boxConfig"].is_object())
        return;

    const json& config = reply["boxConfig"];
    if (config.contains("cAutoFeed"))
        obj->GetFilaSystem()->GetAmsSystemSetting().SetDetectOnInsertEnabled(json_number(config, "cAutoFeed") != 0.);
    if (config.contains("cSelfTest"))
        obj->GetFilaSystem()->GetAmsSystemSetting().SetDetectOnPowerupEnabled(json_number(config, "cSelfTest") != 0.);
}

void CrealityPrintAgent::report_loaded_slot(int loaded_slot)
{
    auto*          dev_manager = GUI::wxGetApp().getDeviceManager();
    MachineObject* obj         = dev_manager ? dev_manager->get_my_machine(device_info.dev_id) : nullptr;
    if (obj)
        ExtderSystemParser::ParseLoadedSlot(loaded_slot, obj->GetExtderSystem());
}

void CrealityPrintAgent::on_printer_connected(const std::string& dev_id)
{
    // The device page shows the slots, and Load and Unload need them, without a sync from the sidebar.
    fetch_filament_info_on_main(dev_id);

    std::lock_guard<std::mutex> lock(watch_mutex);
    notice_ip      = device_info.dev_ip;
    notice_api_key = device_info.api_key;
    if (!watch_thread.joinable() && !watch_stop)
        watch_thread = std::thread([this] { watch_printers(); });
}

int CrealityPrintAgent::disconnect_printer()
{
    {
        std::lock_guard<std::mutex> lock(watch_mutex);
        notice_ip.clear();
    }
    return MoonrakerPrinterAgent::disconnect_printer();
}

bool CrealityPrintAgent::start_discovery(bool start, bool sending)
{
    const bool started = MoonrakerPrinterAgent::start_discovery(start, sending);

    std::lock_guard<std::mutex> lock(watch_mutex);
    discovering = start;
    scan_wanted = start;
    if (start && !watch_thread.joinable() && !watch_stop)
        watch_thread = std::thread([this] { watch_printers(); });
    watch_wake.notify_all();
    return started;
}

bool CrealityPrintAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode /*sync_mode*/)
{
    if (device_info.dev_ip.empty()) {
        BOOST_LOG_TRIVIAL(warning)
            << "CrealityPrintAgent::fetch_filament_info: no device IP, falling back to base agent";
        return MoonrakerPrinterAgent::fetch_filament_info(std::move(dev_id));
    }

    CrealityPrint host = make_host(device_info.dev_ip, device_info.api_key);

    // Only a K1-family `box` object is known to take the commands Load, Unload and the slot refresh
    // send, and it alone names the slot feeding the toolhead, whichever way the slots are read.
    const std::string        box_response = host.is_k1_family_board() ? query_box_object() : std::string();
    const std::optional<int> loaded_slot  = parse_loaded_slot(box_response);
    can_command_box                       = loaded_slot.has_value();

    const auto defer_to_base = [&] {
        const bool synced = MoonrakerPrinterAgent::fetch_filament_info(dev_id);
        if (synced && loaded_slot)
            report_loaded_slot(*loaded_slot);
        return synced;
    };

    // Defer to base if this isn't a K-series board with CFS firmware support.
    if (!host.supports_multi_color_print()) {
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: " << host.model_name()
            << " is not CFS-capable, deferring to base Moonraker agent";
        return defer_to_base();
    }

    BOOST_LOG_TRIVIAL(info)
        << "CrealityPrintAgent: querying CFS slots on " << host.model_name();

    const std::string response = host.query_boxes_info();

    std::vector<CFSSlot> slots;
    int                  box_count = 0;
    std::string          parse_err;
    if (!parse_cfs_response(response, slots, box_count, parse_err)) {
        BOOST_LOG_TRIVIAL(warning)
            << "CrealityPrintAgent: CFS query failed (" << parse_err << "), "
            << "falling back to base agent";
        return defer_to_base();
    }

    if (box_count == 0) {
        // No active CFS boxes attached — printer is in direct-spool mode. Let the
        // base agent take over so the user still gets whatever filament info
        // Moonraker exposes.
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: no active CFS boxes, deferring to base agent";
        return defer_to_base();
    }

    add_slots_from_box_object(box_response, slots);
    if (std::any_of(slots.begin(), slots.end(), [](const CFSSlot& s) { return s.vendor.empty() && !s.material_code.empty(); })) {
        if (material_library.empty())
            material_library = host.query_material_library();
        name_slots_from_library(material_library, slots);
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
            tray.remain       = s.remain;

            if (bundle) {
                tray.tray_info_idx = match_filament_preset(
                    bundle->filaments, s.vendor, s.brand_name, tray.tray_type);
            }

            trays.push_back(std::move(tray));
        }
    }

    // A CFS is a four-slot box like the AMS, and the AMS view draws each spool's level, which
    // the AMS-Lite view the other Moonraker agents use does not.
    box_climate = box_climates(response);
    build_ams_payload(box_count, max_slots - 1, trays, DevAmsType::AMS);
    if (loaded_slot)
        report_loaded_slot(*loaded_slot);
    if (can_command_box)
        report_box_config(host.query_box_config());
    return true;
}

std::string CrealityPrintAgent::change_filament_gcode(bool load, int ams_id, int slot_id, int nozzle_temp)
{
    // The box's own Tn_extrude_temp in the stock K1 box.cfg, used when the device page knows no
    // temperature for the filaments involved.
    constexpr int default_temp = 220;
    const int     temp         = nozzle_temp > 0 ? nozzle_temp : default_temp;

    // Both cut the filament at a fixed position, so the toolhead has to be homed. T<n> then cuts
    // what is loaded, feeds slot n and flushes it; BOX_QUIT_MATERIAL cuts and pulls the filament
    // back into its box. Slot n counts four per box across the chain. The nozzle goes off at the
    // end, as it does after the printer's own feed: it would otherwise stay hot until the printer
    // idles out.
    std::string gcode = "IF_NEED_HOME\nM109 S" + std::to_string(temp) + "\n";
    gcode += load ? "T" + std::to_string(ams_id * 4 + slot_id) : std::string("BOX_QUIT_MATERIAL");
    return gcode + "\nM104 S0";
}

std::string CrealityPrintAgent::refresh_slot_gcode(int ams_id, int slot_id)
{
    // ADDR counts boxes from 1; NUM is a bit mask of the slots, A being 1.
    return "BOX_INFO_REFRESH ADDR=" + std::to_string(ams_id + 1) + " NUM=" + std::to_string(1 << slot_id);
}

// Walks boxsInfo exactly like parse_cfs_response() (active type-0 boxes, in order), but returns
// the slot record whatever its state: a write to a slot without a spool is still a write.
bool CrealityPrintAgent::find_cfs_material(const std::string& response,
                                           int                cfs_index,
                                           int                slot_id,
                                           int&               box_id,
                                           json&              material)
{
    const json resp = json::parse(response, nullptr, false);
    if (resp.is_discarded() || !resp.contains("boxsInfo") || !resp["boxsInfo"].contains("materialBoxs"))
        return false;

    int index = 0;
    for (const auto& box : resp["boxsInfo"]["materialBoxs"]) {
        if (box.value("state", 0) != 1 || box.value("type", 0) != 0)
            continue;
        if (index++ != cfs_index)
            continue;
        if (!box.contains("materials") || !box["materials"].is_array())
            return false;
        for (const auto& mat : box["materials"]) {
            if (mat.value("id", -1) == slot_id) {
                box_id   = box.value("id", 0);
                material = mat;
                return true;
            }
        }
        return false;
    }
    return false;
}

// The retMaterials fields used here (numbers may also come as numeric strings):
//   { "base": { "id": "00001", "brand": "Generic", "meterialType": "PLA", "name": "Generic PLA",
//               "minTemp": 190, "maxTemp": 240, ... },
//     "kvParam": { "pressure_advance": 0.04, ... } }
// "meterialType" is the firmware's spelling; "materialType" is accepted as well.
bool CrealityPrintAgent::apply_material_library(const std::string& response, CFSMaterial& material)
{
    const json resp = json::parse(response, nullptr, false);
    if (resp.is_discarded() || !resp.contains("retMaterials") || !resp["retMaterials"].is_array())
        return false;

    const std::string type_lower = to_lower(material.filament_type);
    const std::string base_lower = to_lower(normalize_filament_type(material.filament_type));
    const std::string name_lower = to_lower(material.name);

    auto entry_type = [](const json& base) {
        std::string t = json_string(base, "meterialType");
        return t.empty() ? json_string(base, "materialType") : t;
    };

    // 1: same name, 2: one name contains the other (closest length wins), 3: the generic entry
    // of the same type, exact type before base type.
    const json* best      = nullptr;
    int         best_rank = 4;
    size_t      best_diff = std::string::npos;
    for (const auto& entry : resp["retMaterials"]) {
        if (!entry.is_object() || !entry.contains("base") || !entry["base"].is_object())
            continue;
        const json&       base      = entry["base"];
        const std::string etype     = to_lower(entry_type(base));
        const std::string ename     = to_lower(json_string(base, "name"));
        const bool        same_type = etype == type_lower;
        const bool        same_base = same_type || to_lower(normalize_filament_type(entry_type(base))) == base_lower;
        if (ename.empty() || !same_base)
            continue;

        int    rank = 4;
        size_t diff = 0;
        if (ename == name_lower) {
            rank = 1;
        } else if (!name_lower.empty()
                   && (ename.find(name_lower) != std::string::npos || name_lower.find(ename) != std::string::npos)) {
            rank = 2;
            diff = ename.size() > name_lower.size() ? ename.size() - name_lower.size() : name_lower.size() - ename.size();
        } else if (to_lower(json_string(base, "brand")) == "generic") {
            rank = 3;
            diff = same_type ? 0 : 1;
        }
        if (rank < best_rank || (rank == best_rank && diff < best_diff)) {
            best      = &entry;
            best_rank = rank;
            best_diff = diff;
        }
    }
    if (best == nullptr)
        return false;

    const json& base = (*best)["base"];
    material.rfid          = json_string(base, "id");
    material.vendor        = json_string(base, "brand");
    material.filament_type = entry_type(base);
    material.name          = json_string(base, "name");
    if (material.min_temp == 0)
        material.min_temp = static_cast<int>(json_number(base, "minTemp"));
    if (material.max_temp == 0)
        material.max_temp = static_cast<int>(json_number(base, "maxTemp"));
    if (material.pressure_advance == 0. && best->contains("kvParam") && (*best)["kvParam"].is_object())
        material.pressure_advance = json_number((*best)["kvParam"], "pressure_advance");
    return true;
}

json CrealityPrintAgent::build_modify_material(int box_id, int slot_id, const CFSMaterial& material)
{
    if (material.is_clear())
        return {{"boxId", box_id}, {"id", slot_id}, {"rfid", ""}, {"type", ""}, {"vendor", ""}, {"name", ""},
                {"color", ""}, {"minTemp", 0}, {"maxTemp", 0}, {"pressure", 0}};

    // The printer stores colours as "#0rrggbb". Creality Print sends the temperatures as
    // floats (it adds 1e-8); that is mirrored so the firmware gets what its own client sends.
    const std::string rgb = color_rgb(material.color_hex);
    return {{"boxId", box_id},
            {"id", slot_id},
            {"rfid", material.rfid},
            {"type", material.filament_type},
            {"vendor", material.vendor},
            {"name", material.name},
            {"color", rgb.empty() ? std::string() : "#0" + rgb},
            {"minTemp", material.min_temp + 1e-8},
            {"maxTemp", material.max_temp + 1e-8},
            {"pressure", material.pressure_advance}};
}

bool CrealityPrintAgent::material_matches(const json& slot, const CFSMaterial& material)
{
    const std::string vendor = json_string(slot, "vendor");
    const std::string type   = json_string(slot, "type");
    const std::string name   = json_string(slot, "name");
    if (material.is_clear())
        return slot.value("state", 0) == 0 || (vendor.empty() && type.empty() && name.empty());

    if (to_lower(type) != to_lower(material.filament_type) || to_lower(name) != to_lower(material.name))
        return false;
    if (!material.vendor.empty() && to_lower(vendor) != to_lower(material.vendor))
        return false;
    const std::string rgb = color_rgb(material.color_hex);
    return rgb.empty() || color_rgb(json_string(slot, "color")) == rgb;
}

int CrealityPrintAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    const json msg = json::parse(json_str, nullptr, false);
    if (is_ams_filament_setting(msg))
        return set_cfs_slot(msg["print"]);
    return MoonrakerPrinterAgent::send_message(std::move(dev_id), std::move(json_str), qos, flag);
}

int CrealityPrintAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    const json j = json::parse(json_str, nullptr, false);
    if (is_ams_filament_setting(j))
        return set_cfs_slot(j["print"]);

    if (const json fields = can_command_box && !j.is_discarded() && j.contains("print") ? box_config_fields(j["print"]) : json::object();
        !fields.empty()) {
        // The settings page calls this from the main thread.
        std::thread([dev_ip = device_info.dev_ip, api_key = device_info.api_key, fields] {
            if (!make_host(dev_ip, api_key).set_box_config(fields))
                BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: the CFS did not confirm " << fields.dump();
        }).detach();
        return BAMBU_NETWORK_SUCCESS;
    }

    if (!can_command_box || j.is_discarded() || !j.contains("print") || j["print"].value("command", "") != "ams_change_filament")
        return MoonrakerPrinterAgent::send_message_to_printer(std::move(dev_id), std::move(json_str), qos, flag);

    const auto& cmd     = j["print"];
    const int   ams_id  = cmd.value("ams_id", -1);
    const int   slot_id = cmd.value("slot_id", -1);
    // An unload marks itself with slot 255; the external spool and other virtual trays sit above
    // the 16 boxes' ids and have no box to load them.
    const bool  load    = slot_id != 255;
    if (load && (ams_id < 0 || ams_id >= 16 || slot_id < 0 || slot_id > 3))
        return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;

    const int         temp  = std::max(cmd.value("curr_temp", -1), cmd.value("tar_temp", -1));
    const std::string gcode = change_filament_gcode(load, ams_id, slot_id, temp);
    BOOST_LOG_TRIVIAL(info) << "CrealityPrintAgent: " << (load ? "loading" : "unloading") << " the CFS: " << gcode;

    // Heating, cutting and flushing take minutes; the slots are read back once the box is done.
    send_gcode_async(dev_id, gcode, [this, dev_id](bool success) {
        if (!success)
            BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: the CFS did not finish changing filament";
        fetch_filament_info_on_main(dev_id);
    }, 600);
    return BAMBU_NETWORK_SUCCESS;
}

int CrealityPrintAgent::command_ams_refresh_rfid(std::string dev_id, int ams_id, int slot_id, int sequence_id, bool lan_mode)
{
    // The device page's older form names no box and counts the slot across every box.
    if (ams_id < 0 && slot_id >= 0) {
        ams_id  = slot_id / 4;
        slot_id = slot_id % 4;
    }
    if (!can_command_box || ams_id < 0 || slot_id < 0 || slot_id > 3)
        return MoonrakerPrinterAgent::command_ams_refresh_rfid(std::move(dev_id), ams_id, slot_id, sequence_id, lan_mode);

    const std::string gcode = refresh_slot_gcode(ams_id, slot_id);
    BOOST_LOG_TRIVIAL(info) << "CrealityPrintAgent: reading a CFS tag: " << gcode;
    send_gcode_async(dev_id, gcode, [this, dev_id](bool success) {
        if (!success)
            BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: the CFS did not read the slot's tag again";
        fetch_filament_info_on_main(dev_id);
    }, 120);
    return BAMBU_NETWORK_SUCCESS;
}

// Device tab slot edit -> modifyMaterial. Runs on the caller's thread and blocks for the
// write and its read-back, like fetch_filament_info() does for the read.
int CrealityPrintAgent::set_cfs_slot(const json& cmd)
{
    const int cfs_index = static_cast<int>(json_number(cmd, "ams_id", -1.));
    const int slot_id   = static_cast<int>(json_number(cmd, "slot_id", json_number(cmd, "tray_id", -1.)));
    if (device_info.dev_ip.empty() || cfs_index < 0 || slot_id < 0) {
        BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: ams_filament_setting without a printer or slot: " << cmd.dump();
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    CrealityPrint host = make_host(device_info.dev_ip, device_info.api_key);
    // Only these boards are known to take modifyMaterial; other models keep reporting that slot
    // writes are not available.
    if (!host.supports_cfs_write())
        return ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE;

    // The external spool holder (virtual tray) is not published as a CFS slot, so it has no
    // CFS address either.
    int  box_id = 0;
    json current;
    if (!find_cfs_material(host.query_boxes_info(), cfs_index, slot_id, box_id, current)) {
        BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: no CFS slot " << cfs_index << "/" << slot_id;
        return ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE;
    }

    // The Device tab's Reset sends neither a filament id nor a type: that clears the slot.
    CFSMaterial       material;
    const std::string filament_id = json_string(cmd, "tray_info_idx");
    const std::string tray_type   = json_string(cmd, "tray_type");
    if (!filament_id.empty() || !tray_type.empty()) {
        const std::string rgb  = color_rgb(json_string(cmd, "tray_color"));
        material.filament_type = tray_type;
        material.color_hex     = rgb.empty() ? std::string() : "#" + rgb;
        material.min_temp      = static_cast<int>(json_number(cmd, "nozzle_temp_min"));
        material.max_temp      = static_cast<int>(json_number(cmd, "nozzle_temp_max"));

        auto* bundle = GUI::wxGetApp().preset_bundle;
        if (const Preset* preset = bundle ? find_filament_preset(bundle->filaments, filament_id) : nullptr) {
            const auto& config = preset->config;
            if (const auto* vendor = config.option<ConfigOptionStrings>("filament_vendor"); vendor && !vendor->values.empty())
                material.vendor = vendor->values.front();
            if (material.filament_type.empty())
                if (const auto* type = config.option<ConfigOptionStrings>("filament_type"); type && !type->values.empty())
                    material.filament_type = type->values.front();
            if (const auto* pa = config.option<ConfigOptionFloats>("pressure_advance"); pa && !pa->values.empty())
                material.pressure_advance = pa->values.front();
            material.name = preset->name.substr(0, preset->name.find('@'));
            while (!material.name.empty() && material.name.back() == ' ')
                material.name.pop_back();
        }
        if (material.filament_type.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: ams_filament_setting without a filament type: " << cmd.dump();
            return BAMBU_NETWORK_ERR_INVALID_RESULT;
        }
        if (material.vendor.empty())
            material.vendor = "Generic";
        if (material.name.empty())
            material.name = material.vendor + " " + material.filament_type;

        // The printer keys a slot by its library id; without the library the preset's own
        // identity is written.
        if (!apply_material_library(host.query_material_library(), material))
            BOOST_LOG_TRIVIAL(info) << "CrealityPrintAgent: no material library entry for " << material.name;

        if (material.min_temp <= 0 || material.max_temp <= 0) {
            BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: refusing to write " << material.name << " without a nozzle temperature range";
            return BAMBU_NETWORK_ERR_INVALID_RESULT;
        }
    }

    // A K1-family box applies only part of a clear, and leaves the slot's tag unread. Reading the
    // tag again gives the slot back what the spool says.
    if (material.is_clear() && can_command_box)
        return command_ams_refresh_rfid(device_info.dev_id, cfs_index, slot_id, 0, true);

    if (!host.modify_material(build_modify_material(box_id, slot_id, material)))
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;

    // Verify by reading the slot back; some firmwares apply the change a moment later.
    std::string response;
    bool        applied = false;
    for (int attempt = 0; attempt < 3 && !applied; ++attempt) {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        response = host.query_boxes_info();
        int  unused_box_id = 0;
        json slot;
        applied = find_cfs_material(response, cfs_index, slot_id, unused_box_id, slot) && material_matches(slot, material);
    }

    BOOST_LOG_TRIVIAL(info) << "CrealityPrintAgent: CFS slot " << box_id << "/" << slot_id << " "
                            << (material.is_clear() ? std::string("clear") : material.vendor + " " + material.name + " " + material.color_hex)
                            << (applied ? ": applied" : ": NOT applied");

    // Read the slots again, so the device page shows what the printer holds either way.
    fetch_filament_info_on_main(device_info.dev_id);

    if (!applied) {
        GUI::wxGetApp().CallAfter([] {
            GUI::show_error(nullptr, _L("The printer did not apply the CFS slot change. Make sure it is idle and that the "
                                        "slot does not hold an RFID spool, then try again."));
        });
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }
    return BAMBU_NETWORK_SUCCESS;
}

} // namespace Slic3r

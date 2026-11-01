#ifndef __CREALITY_PRINT_AGENT_HPP__
#define __CREALITY_PRINT_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {

class PresetCollection;

// Filament sync for Creality K-series printers with CFS.
//
// Inherits MoonrakerPrinterAgent for all communication / certificates / discovery /
// binding / print-job operations. Overrides fetch_filament_info() to query the
// K-series CFS over its port-9999 WebSocket, convert each loaded slot to an
// AmsTrayData entry, and publish via the base-class build_ams_payload() — the
// same shape used by QidiPrinterAgent and SnapmakerPrinterAgent.
//
// Model detection delegated to CrealityPrint::supports_multi_color_print() (PR #13291).
// For non-CFS K-series boards or when the WS query fails, falls back to the base
// MoonrakerPrinterAgent behaviour.
//
// Slot edits made in the Device tab arrive as Orca's "ams_filament_setting" command;
// they are written back to the CFS with the same "modifyMaterial" command Creality
// Print sends, then read back to verify and republish the slots.

class CrealityPrintAgent final : public MoonrakerPrinterAgent
{
public:
    struct CFSSlot
    {
        int         box_id  = 0;   // CFS unit index (0 for first box, 1 for chained second box)
        int         printer_box_id = 0; // The printer's own id for that unit
        int         slot_id = 0;   // Slot index within the box (0..3)
        std::string color_hex;     // "#RRGGBB"
        std::string filament_type; // "PLA", "ABS", "PETG", ... or "" if the printer cannot name the spool
        std::string brand_name;    // "Hyper PLA", ... or ""
        std::string vendor;        // "Creality", "eSUN", or "" if unknown
        int         remain = -1;   // Filament left on the spool, 0..100; -1 when the box does not report it
        std::string material_code; // The box object's material code ("106003"), for a slot read from there
    };

    // A slot record to write back. All fields empty clears the slot.
    struct CFSMaterial
    {
        std::string rfid;          // material library id ("00001" = Generic PLA), "" if unknown
        std::string vendor;
        std::string filament_type;
        std::string name;
        std::string color_hex;     // "#RRGGBB"
        int         min_temp = 0;
        int         max_temp = 0;
        double      pressure_advance = 0.;

        bool is_clear() const { return vendor.empty() && filament_type.empty() && name.empty() && color_hex.empty(); }
    };

    explicit CrealityPrintAgent(std::string log_dir);
    ~CrealityPrintAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    // The device page's Load and Unload, carried out by a K1-family board's box. Other boards and
    // commands go to the base agent.
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;
    // The device page's per-slot refresh: a K1-family box reads the slot's tag again before the
    // slots are read back.
    int command_ams_refresh_rfid(std::string dev_id, int ams_id, int slot_id, int sequence_id, bool lan_mode) override;

    // The send dialog's colorMatch_<n> entries for a print's ams_mapping_info, one per filament
    // mapped onto a CFS slot of `slots`. Empty when no filament is mapped onto a slot, or one is
    // mapped onto the spool holder, which the firmware cannot mix with the CFS. Nothing when a
    // mapped slot is not in `slots`.
    static std::optional<std::map<std::string, std::string>> color_match_info(const std::string&          ams_mapping_info,
                                                                              const std::vector<CFSSlot>& slots);

    // The box's own Auto Refill groups, as the device page's filam_bak bit masks of slot indexes,
    // from its `same_material` field: [[code, colour, ["T1A", "T1C"], type], ...].
    static std::vector<int> refill_groups(const nlohmann::json& same_material);
    // Adds to `print`, the status report the device page reads, the Auto Refill state of the
    // Klipper `box` object. A box that does not report one is left out.
    static void add_refill_status(const nlohmann::json& box, nlohmann::json& print);

    // The boxConfig fields a device page settings command writes, or an empty object for any other
    // command. "Insertion update" is the box's cAutoFeed and "Update on startup" its cSelfTest,
    // both of which read the spools' tags.
    static nlohmann::json box_config_fields(const nlohmann::json& print_cmd);

    // The script a K1-family box runs to load slot `slot_id` of box `ams_id` (both from 0), or to
    // unload whatever the toolhead holds when `load` is false. `nozzle_temp` is the temperature
    // the filament moves at; 0 or less takes the box's own default.
    static std::string change_filament_gcode(bool load, int ams_id, int slot_id, int nozzle_temp);
    // The script a K1-family box runs to read the tag of slot `slot_id` of box `ams_id` again.
    static std::string refresh_slot_gcode(int ams_id, int slot_id);

    // The slot index, four per box, whose filament a K1-family box reports in the toolhead, -1 when
    // it reports none, and nothing when the reply names no box.
    static std::optional<int> parse_loaded_slot(const std::string& response);

    // The message of a line the printer writes to its G-code console when something fails, as
    // shown on its screen, or an empty string for any other line. Stock macros call a command
    // rooted firmware lacks on every print, so "Unknown command" (key61) is left out.
    static std::string printer_error_text(const std::string& line);

    // The notice number in a printer status report (see CrealityPrint::query_printer_status()), 0
    // when its screen shows none and -1 for a reply that does not say.
    static int printer_notice(const std::string& response);

    // The Moonraker URL to try beside one that names no port, or empty when it names one.
    static std::string moonraker_base_url(const std::string& base_url);

    // The humidity level of a box, from 1 to 5 with 5 the driest, in the bands of Bambu's percentage AMS.
    static int humidity_level(double percent);
    // The AMS fields for the temperature and humidity of each CFS box in a boxsInfo reply, numbered
    // like parse_cfs_response() numbers the boxes. A sensor that reads no humidity adds none.
    static std::vector<nlohmann::json> box_climates(const std::string& response);

    // Parse the boxsInfo JSON returned by CrealityPrint::query_boxes_info() into
    // a flat list of loaded slots, plus the count of CFS boxes the printer reports.
    static bool parse_cfs_response(const std::string&    response,
                                   std::vector<CFSSlot>& slots,
                                   int&                  box_count,
                                   std::string&          error);

    // Port 9999 leaves a slot blank when its spool has no Creality tag, though a K1-family box knows
    // the spool from the tag it read. Adds to `slots` each one that the Klipper `box` object lists in
    // `same_material`, which drops a slot as soon as its spool runs out.
    static void add_slots_from_box_object(const std::string& box_response, std::vector<CFSSlot>& slots);
    // Names those slots from the printer's material library (the retMaterials answer to
    // reqMaterials), whose ids are the material code without its first digit.
    static void name_slots_from_library(const std::string& library_response, std::vector<CFSSlot>& slots);

    // Strip PLA/PETG/... subtype suffixes ("PLA Silk", "PLA+", "ABS Pro") to base
    // type so the preset_bundle->filaments.filament_id_by_type() lookup succeeds.
    // A fibre-filled type ("PETG-CF") stays whole: it is a material of its own, with its own presets.
    static std::string normalize_filament_type(const std::string& filament_type);

    // Score visible compatible filament presets against the CFS spool metadata and
    // return the best-matching filament_id. See implementation for scoring details.
    static std::string match_filament_preset(const PresetCollection& filaments,
                                             const std::string&      vendor,
                                             const std::string&      brand_name,
                                             const std::string&      base_type);

    // Whether a change to the Klipper `box` object alters what the device page shows: the slot
    // feeding the toolhead or the spools in the slots. `seen` keeps those fields between calls; the
    // first sight of a field is not a change, since the sync on connecting already read it.
    static bool box_update_changes_slots(const nlohmann::json& box_update, std::map<std::string, std::string>& seen);

    // Find the raw material record of one slot in a boxsInfo response, whatever its state,
    // addressed the way fetch_filament_info() publishes it: cfs_index counts the active CFS
    // boxes in order, slot_id is the slot within the box. box_id receives the printer's own
    // box id, which is what modifyMaterial expects.
    static bool find_cfs_material(const std::string& response,
                                  int                cfs_index,
                                  int                slot_id,
                                  int&               box_id,
                                  nlohmann::json&    material);

    // Take the slot identity (library id, vendor, type, name) from the printer's material
    // library (the retMaterials answer to reqMaterials): the entry with the preset's name,
    // else the generic entry of the same type. Returns false when nothing fits.
    static bool apply_material_library(const std::string& response, CFSMaterial& material);

    // The modifyMaterial parameters for one slot.
    static nlohmann::json build_modify_material(int box_id, int slot_id, const CFSMaterial& material);

    // Whether a raw slot record (see find_cfs_material) holds what was written.
    static bool material_matches(const nlohmann::json& slot, const CFSMaterial& material);

protected:
    void on_printer_connected(const std::string& dev_id) override;
    int  disconnect_printer() override;
    void add_ams_unit_status(int ams_id, nlohmann::json& ams_unit) const override;
    std::string alternative_base_url(const std::string& base_url) const override;
    void add_status_objects(const std::set<std::string>& available, std::set<std::string>& subscribe) const override;
    void on_status_update(const std::string& dev_id, const nlohmann::json& updates) override;
    void on_gcode_response(const std::string& dev_id, const std::string& line) override;
    void add_print_status(const nlohmann::json& status, nlohmann::json& print) const override;
    bool print_uploaded_file(const PrintParams& params, const std::string& filename) override;

private:
    // Tells the device page which slot feeds the toolhead.
    void report_loaded_slot(int loaded_slot);
    // Shows `message` on the device page's print progress, or clears it when empty.
    void report_printer_message(std::string message);
    // Shows notice `key` on the print progress, or clears notice `previous_key` when `key` is 0.
    void report_printer_notice(int key, int previous_key);
    // Until the agent is destroyed, asks a K1-family printer for the notice on its screen. Moonraker
    // does not carry those notices, and port 9999 is only read when something asks it.
    void watch_printers();
    // Tells the device page how the box reads tags, from a boxConfig answer.
    void report_box_config(const std::string& response);
    int  set_cfs_slot(const nlohmann::json& print_cmd);

    // Whether the last filament sync found a K1-family board's box, the only CFS this agent knows
    // how to command. Other boards may publish a `box` object too, but none has been tried.
    std::atomic<bool> can_command_box{false};
    // Touched only by the status stream's thread.
    std::map<std::string, std::string> seen_box_fields;
    // The printer's material library, kept once a slot has needed it. Touched only on the main thread.
    std::string material_library;
    // What the last slot read found for each box, in AMS order.
    std::vector<nlohmann::json> box_climate;

    std::thread             watch_thread;
    std::mutex              watch_mutex;
    std::condition_variable watch_wake;
    bool                    watch_stop = false;
    std::string             notice_ip;  // Empty while no printer is connected
    std::string             notice_api_key;
};

} // namespace Slic3r

#endif

#ifndef __CREALITY_PRINT_AGENT_HPP__
#define __CREALITY_PRINT_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <map>
#include <optional>
#include <set>
#include <string>
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
    };

    explicit CrealityPrintAgent(std::string log_dir);
    ~CrealityPrintAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

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

    // The script a K1-family box runs to load slot `slot_id` of box `ams_id` (both from 0), or to
    // unload whatever the toolhead holds when `load` is false. `nozzle_temp` is the temperature
    // the filament moves at; 0 or less takes the box's own default.
    static std::string change_filament_gcode(bool load, int ams_id, int slot_id, int nozzle_temp);
    // The script a K1-family box runs to read the tag of slot `slot_id` of box `ams_id` again.
    static std::string refresh_slot_gcode(int ams_id, int slot_id);

    // The slot index, four per box, whose filament a K1-family box reports in the toolhead, -1 when
    // it reports none, and nothing when the reply names no box.
    static std::optional<int> parse_loaded_slot(const std::string& response);

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

protected:
    void on_printer_connected(const std::string& dev_id) override;
    void add_status_objects(const std::set<std::string>& available, std::set<std::string>& subscribe) const override;
    void on_status_update(const std::string& dev_id, const nlohmann::json& updates) override;
    void add_print_status(const nlohmann::json& status, nlohmann::json& print) const override;
    bool print_uploaded_file(const PrintParams& params, const std::string& filename) override;

private:
    // Tells the device page which slot feeds the toolhead.
    void report_loaded_slot(int loaded_slot);

    // Whether the last filament sync found a K1-family board's box, the only CFS this agent knows
    // how to command. Other boards may publish a `box` object too, but none has been tried.
    std::atomic<bool> can_command_box{false};
    // Touched only by the status stream's thread.
    std::map<std::string, std::string> seen_box_fields;
};

} // namespace Slic3r

#endif

#ifndef __CREALITY_PRINT_AGENT_HPP__
#define __CREALITY_PRINT_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"

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
        int         slot_id = 0;   // Slot index within the box (0..3)
        std::string color_hex;     // "#RRGGBB"
        std::string filament_type; // "PLA", "ABS", "PETG", ...
        std::string brand_name;    // "Hyper PLA", ...
        std::string vendor;        // "Creality", "eSUN", or "" if unknown
        int         remain = -1;   // Filament left on the spool, 0..100; -1 when the box does not report it
    };

    explicit CrealityPrintAgent(std::string log_dir);
    ~CrealityPrintAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    // Parse the boxsInfo JSON returned by CrealityPrint::query_boxes_info() into
    // a flat list of loaded slots, plus the count of CFS boxes the printer reports.
    static bool parse_cfs_response(const std::string&    response,
                                   std::vector<CFSSlot>& slots,
                                   int&                  box_count,
                                   std::string&          error);

    // Strip PLA/PETG/... subtype suffixes ("PLA Silk", "PLA+", "ABS Pro") to base
    // type so the preset_bundle->filaments.filament_id_by_type() lookup succeeds.
    static std::string normalize_filament_type(const std::string& filament_type);

    // Score visible compatible filament presets against the CFS spool metadata and
    // return the best-matching filament_id. See implementation for scoring details.
    static std::string match_filament_preset(const PresetCollection& filaments,
                                             const std::string&      vendor,
                                             const std::string&      brand_name,
                                             const std::string&      base_type);

    // Parse the Klipper `box` object a K1-family board publishes over Moonraker into the
    // same slot list parse_cfs_response() produces from the K2 platform's boxsInfo.
    static bool parse_box_response(const std::string&    response,
                                   std::vector<CFSSlot>& slots,
                                   int&                  box_count,
                                   std::string&          error);

private:
    // Read the Klipper `box` object over Moonraker. Empty when the printer has no such
    // object, which is every board whose CFS only answers the boxsInfo WebSocket.
    std::string query_box_object() const;

    // Turn a parsed slot list into the AMS payload the filament views read.
    void publish_cfs_slots(const std::vector<CFSSlot>& slots, int box_count);
};

} // namespace Slic3r

#endif

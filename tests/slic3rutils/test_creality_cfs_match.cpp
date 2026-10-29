#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/CrealityPrintAgent.hpp"

using namespace Slic3r;

namespace {

// A standalone filament collection, built the same way PresetBundle builds its own, so the
// CFS matcher can be exercised without loading the shipped profiles or touching a printer.
struct FilamentTestCollection : public PresetCollection
{
    FilamentTestCollection()
        : PresetCollection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
    {}
};

struct FilamentSpec
{
    const char *name;
    const char *filament_id;
    const char *filament_type;
    bool        is_library = false;   // belongs to the Orca Filament Library, not the printer vendor
    bool        is_system  = true;
};

// Load the specs into the collection, then stamp the fields the matcher reads. Done in a second
// pass because load_preset() keeps m_presets sorted, so a reference taken during the first pass
// can be left dangling by a later load.
void populate(PresetCollection &filaments, const std::vector<FilamentSpec> &specs,
              const VendorProfile &vendor, const VendorProfile &library)
{
    for (const FilamentSpec &spec : specs) {
        DynamicPrintConfig config(filaments.default_preset().config);
        config.option<ConfigOptionStrings>("filament_type", true)->values = {spec.filament_type};
        filaments.load_preset(std::string(), spec.name, config, /*select=*/false);
    }
    for (auto it = filaments.begin(); it != filaments.end(); ++it) {
        const auto spec = std::find_if(specs.begin(), specs.end(),
                                       [&](const FilamentSpec &s) { return it->name == s.name; });
        if (spec == specs.end())
            continue;
        it->filament_id  = spec->filament_id;
        it->vendor       = spec->is_library ? &library : &vendor;
        it->is_system    = spec->is_system;
        it->is_default   = false;
        it->is_visible   = true;
        it->is_compatible = true;
    }
}

// What a stock K2 with a 0.4 nozzle sees after the filament_id rework: the vendor's plain generics
// carry no "Creality" in their name (they are "Generic <material> @<scope>"), while the subtype
// variants that do keep a vendor scope are separate products with their own filament_ids.
const std::vector<FilamentSpec> k2_stock_nozzle{
    {"AliZ PLA @System",                       "ALIZ-PLA",     "PLA",  /*is_library=*/true},
    {"Generic PETG @K2-all",                   "PETG-GENERIC", "PETG"},
    {"Generic PLA @K2-all",                    "PLA-GENERIC",  "PLA"},
    {"Generic PLA High Speed @Creality K2-all","PLA-HS",       "PLA"},
    {"Generic PLA Matte @Creality K2-all",     "PLA-MATTE",    "PLA"},
    {"Generic PLA Silk @Creality K2-all",      "PLA-SILK",     "PLA"},
    {"Hyper PLA @K2-all",                      "PLA-HYPER",    "PLA"},
};

std::string match(const std::vector<FilamentSpec> &specs, const std::string &spool_vendor,
                  const std::string &spool_name, const std::string &base_type)
{
    FilamentTestCollection filaments;
    VendorProfile          vendor("Creality");
    VendorProfile          library(PresetBundle::ORCA_FILAMENT_LIBRARY);
    vendor.name  = "Creality";
    library.name = PresetBundle::ORCA_FILAMENT_LIBRARY;
    populate(filaments, specs, vendor, library);
    return CrealityPrintAgent::match_filament_preset(filaments, spool_vendor, spool_name, base_type);
}

} // namespace

// Orca: a CFS spool that names no recognised product must map to the vendor's plain generic. The
// subtype variants ("High Speed", "Matte", "Silk") score just as well on vendor alone, and since
// each is its own product with its own filament_id, letting one of them win sends the printer the
// id of a filament the user does not have loaded.
TEST_CASE("An unbranded CFS spool maps to the vendor's plain generic, not a subtype", "[CFS][Creality]")
{
    CHECK(match(k2_stock_nozzle, "Creality", "", "PLA") == "PLA-GENERIC");
}

// Orca: the vendor bonus reads the preset's owning VendorProfile. "Generic PETG @K2-all" does not
// repeat "Creality" anywhere in its name, so a name based vendor test scored nothing for it and the
// spool fell through to the collection wide first-of-type - an unrelated third party PETG.
TEST_CASE("A CFS spool matches its vendor's presets even when the name omits the vendor", "[CFS][Creality]")
{
    CHECK(match(k2_stock_nozzle, "Creality", "", "PETG") == "PETG-GENERIC");
}

TEST_CASE("A branded CFS spool still beats the generic", "[CFS][Creality]")
{
    CHECK(match(k2_stock_nozzle, "Creality", "Hyper PLA", "PLA") == "PLA-HYPER");
}

// Orca: the specificity penalty must not stop a spool that genuinely asks for a subtype from
// getting it - only unclaimed qualifiers are penalised.
TEST_CASE("A CFS spool that names a subtype gets that subtype", "[CFS][Creality]")
{
    CHECK(match(k2_stock_nozzle, "Creality", "Generic PLA Silk", "PLA") == "PLA-SILK");
    CHECK(match(k2_stock_nozzle, "Creality", "Generic PLA Matte", "PLA") == "PLA-MATTE");
}

// Orca: a third party spool matches no preset by brand or vendor, so it falls back to the first
// visible preset of the same type rather than returning nothing. Which one that is depends on the
// collection's ordering, so only the type is pinned here - what matters is that a PLA spool never
// comes back empty and never comes back as another material.
TEST_CASE("A CFS spool from an unknown vendor falls back to a preset of the same type", "[CFS][Creality]")
{
    const std::string matched = match(k2_stock_nozzle, "SomeOtherBrand", "", "PLA");
    REQUIRE_FALSE(matched.empty());
    const auto spec = std::find_if(k2_stock_nozzle.begin(), k2_stock_nozzle.end(),
                                   [&](const FilamentSpec &s) { return matched == s.filament_id; });
    REQUIRE(spec != k2_stock_nozzle.end());
    CHECK(std::string(spec->filament_type) == "PLA");
}


// Orca: Creality's bundle also ships third party filaments ("eSUN PLA+ @K2 Plus-all"). Those carry
// Creality's VendorProfile but name their real brand, so the vendor bonus has to accept a name
// match as well as a profile match - otherwise an eSUN spool stops matching its own preset.
TEST_CASE("A third party spool matches its preset inside the printer vendor's bundle", "[CFS][Creality]")
{
    const std::vector<FilamentSpec> with_third_party{
        {"Generic PLA @K2-all",     "PLA-GENERIC", "PLA"},
        {"eSUN PLA+ @K2 Plus-all",  "ESUN-PLA",    "PLA"},   // shipped by Creality, branded eSUN
    };
    CHECK(match(with_third_party, "eSUN", "", "PLA") == "ESUN-PLA");
    // ... and a Creality spool still prefers Creality's own generic over the eSUN preset, which
    // carries Creality's profile too but adds a brand word the spool never claimed.
    CHECK(match(with_third_party, "Creality", "", "PLA") == "PLA-GENERIC");
}


// Orca: the CFS reports what is left on each spool as "percent". DevAmsTray::remain defaults to
// 0, so a slot whose box never reports a level has to arrive as -1 ("unknown") rather than 0,
// which the AMS views would draw as an empty spool.
TEST_CASE("A CFS slot keeps an unreported filament level unknown", "[CFS][Creality]")
{
    const std::string response = R"({"boxsInfo":{"materialBoxs":[{"id":1,"state":1,"type":0,"materials":[
        {"id":0,"state":1,"vendor":"Creality","type":"PLA","name":"Hyper PLA","color":"#0FF0000","percent":85},
        {"id":1,"state":1,"vendor":"Creality","type":"PETG","name":"CR-PETG","color":"#000FF00"}
    ]}]}})";

    std::vector<CrealityPrintAgent::CFSSlot> slots;
    int                                      box_count = 0;
    std::string                              error;
    REQUIRE(CrealityPrintAgent::parse_cfs_response(response, slots, box_count, error));
    CHECK(error.empty());
    CHECK(box_count == 1);
    REQUIRE(slots.size() == 2);

    CHECK(slots[0].remain == 85);
    CHECK(slots[1].remain == -1);
}


namespace {

// A Moonraker /printer/objects/query?box reply, as a K1-family board with a CFS answers it.
std::string box_reply(const std::string& box_body)
{
    return R"({"result":{"eventtime":1.0,"status":{"box":)" + box_body + "}}}";
}

} // namespace

TEST_CASE("A K1 box slot label gives the box and the slot of the spool", "[CFS][Creality]")
{
    // "T<box><slot>": boxes count from 1 on the printer and from 0 in CFSSlot; slots are letters.
    const std::string response = box_reply(R"({"same_material":[
        ["114001","00B359A",["T1A"],"PLA"],
        ["103001","0000000",["T2C"],"ABS"]
    ]})");

    std::vector<CrealityPrintAgent::CFSSlot> slots;
    int                                      box_count = 0;
    std::string                              error;
    REQUIRE(CrealityPrintAgent::parse_box_response(response, slots, box_count, error));
    REQUIRE(slots.size() == 2);

    CHECK(slots[0].box_id == 0);
    CHECK(slots[0].slot_id == 0);
    CHECK(slots[0].filament_type == "PLA");
    CHECK(slots[1].box_id == 1);
    CHECK(slots[1].slot_id == 2);
    CHECK(slots[1].filament_type == "ABS");
    CHECK(box_count == 2);
}

TEST_CASE("A spool the K1 box lists under several slots fills each of them", "[CFS][Creality]")
{
    const std::string response = box_reply(R"({"same_material":[["114001","00B359A",["T1A","T1D"],"PLA"]]})");

    std::vector<CrealityPrintAgent::CFSSlot> slots;
    int                                      box_count = 0;
    std::string                              error;
    REQUIRE(CrealityPrintAgent::parse_box_response(response, slots, box_count, error));
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].slot_id == 0);
    CHECK(slots[1].slot_id == 3);
    CHECK(box_count == 1);
}

TEST_CASE("A K1 box colour loses the leading zero the printer pads it with", "[CFS][Creality]")
{
    const std::string response = box_reply(R"({"same_material":[["114001","00B359A",["T1A"],"PLA"]]})");

    std::vector<CrealityPrintAgent::CFSSlot> slots;
    int                                      box_count = 0;
    std::string                              error;
    REQUIRE(CrealityPrintAgent::parse_box_response(response, slots, box_count, error));
    REQUIRE(slots.size() == 1);
    CHECK(slots[0].color_hex == "#0B359A");
}

TEST_CASE("A K1 box reports each slot's filament level and keeps an unreported one unknown", "[CFS][Creality]")
{
    // remain_len is per box, indexed by slot, with the values as strings. Box 1 reports
    // slot A; box 2 reports nothing.
    const std::string response = box_reply(R"({"same_material":[
        ["114001","00B359A",["T1A"],"PLA"],
        ["103001","0000000",["T2A"],"ABS"]
    ],"T1":{"remain_len":["72","0","0","0"]}})");

    std::vector<CrealityPrintAgent::CFSSlot> slots;
    int                                      box_count = 0;
    std::string                              error;
    REQUIRE(CrealityPrintAgent::parse_box_response(response, slots, box_count, error));
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].remain == 72);
    CHECK(slots[1].remain == -1);
}

TEST_CASE("A Moonraker printer without a box object reports no CFS", "[CFS][Creality]")
{
    // Moonraker answers a query for an object Klipper does not have with an empty status.
    std::vector<CrealityPrintAgent::CFSSlot> slots;
    int                                      box_count = 0;
    std::string                              error;
    CHECK_FALSE(CrealityPrintAgent::parse_box_response(R"({"result":{"eventtime":1.0,"status":{}}})",
                                                       slots, box_count, error));
    CHECK(slots.empty());
    CHECK(box_count == 0);
    CHECK_FALSE(error.empty());
}

TEST_CASE("Loading a K1 box slot homes, heats and selects the slot across the box chain", "[CFS][Creality]")
{
    // Four slots per box, so slot C of the second box (both counted from 0 here) is T6.
    const std::string gcode = CrealityPrintAgent::change_filament_gcode(true, 1, 2, 240);
    CHECK(gcode.find("IF_NEED_HOME") != std::string::npos);
    CHECK(gcode.find("M109 S240") != std::string::npos);
    CHECK(gcode.find("T6") != std::string::npos);
    CHECK(gcode.find("IF_NEED_HOME") < gcode.find("T6"));
    CHECK(gcode.find("M109") < gcode.find("T6"));
}

TEST_CASE("Unloading the K1 box hands the filament back to the box and turns the nozzle off", "[CFS][Creality]")
{
    const std::string gcode = CrealityPrintAgent::change_filament_gcode(false, 0, 255, 210);
    CHECK(gcode.find("M109 S210") != std::string::npos);
    CHECK(gcode.find("BOX_QUIT_MATERIAL") != std::string::npos);
    CHECK(gcode.find("\nT") == std::string::npos); // no tool change line
    CHECK(gcode.find("M104 S0") > gcode.find("BOX_QUIT_MATERIAL")); // nothing left in the nozzle to keep hot
}

TEST_CASE("A K1 box filament change with no known temperature uses the box's default", "[CFS][Creality]")
{
    CHECK(CrealityPrintAgent::change_filament_gcode(true, 0, 0, -1).find("M109 S220") != std::string::npos);
}

TEST_CASE("Refreshing a K1 box slot addresses the box from 1 and the slot as a bit", "[CFS][Creality]")
{
    CHECK(CrealityPrintAgent::refresh_slot_gcode(0, 0) == "BOX_INFO_REFRESH ADDR=1 NUM=1");
    CHECK(CrealityPrintAgent::refresh_slot_gcode(1, 3) == "BOX_INFO_REFRESH ADDR=2 NUM=8");
}

TEST_CASE("A K1 box names the slot in the toolhead across the box chain", "[CFS][Creality]")
{
    // Each connected box reports its own feeding slot; slot C of box 2 is index 6.
    const std::string response = box_reply(R"({"same_material":[],
        "T1":{"state":"connect","filament":"None"},
        "T2":{"state":"connect","filament":"C"},
        "T3":{"state":"None","filament":"None"}})");
    const auto loaded = CrealityPrintAgent::parse_loaded_slot(response);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == 6);
}

TEST_CASE("A K1 box with nothing in the toolhead reports no loaded slot", "[CFS][Creality]")
{
    const auto loaded = CrealityPrintAgent::parse_loaded_slot(
        box_reply(R"({"same_material":[],"T1":{"state":"connect","filament":"None"}})"));
    REQUIRE(loaded.has_value());
    CHECK(*loaded == -1);
}

TEST_CASE("A reply without a connected K1 box leaves the loaded slot unknown", "[CFS][Creality]")
{
    CHECK_FALSE(CrealityPrintAgent::parse_loaded_slot(box_reply(R"({"same_material":[]})")).has_value());
    CHECK_FALSE(CrealityPrintAgent::parse_loaded_slot("").has_value());
}

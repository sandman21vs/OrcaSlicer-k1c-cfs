#include "CrealityHostDiscovery.hpp"
#include "CrealityPrint.hpp"
#include "cxmdns.h"
#include "Http.hpp"

#include <boost/log/trivial.hpp>
#include <exception>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>
#include <utility>

namespace Slic3r {

namespace {

// Model capability/name lookups live in CrealityPrint (one table shared
// with the print host) so discovery can't drift out of sync again.
bool is_cfs_capable(const std::string& code)
{
    return CrealityPrint::model_supports_multi_color(code);
}

std::string model_name_for(const std::string& code)
{
    return CrealityPrint::model_display_name(code);
}

// Synchronously probe http://<ip>/info for {model, mac}. Short timeout --
// we don't want one slow host to drag down discovery.
void probe_info(CrealityHost& host)
{
    const std::string url = "http://" + host.ip + "/info";
    auto http = Http::get(url);
    http.timeout_connect(2)
        .timeout_max(4)
        .on_complete([&host](std::string body, unsigned /*status*/) {
            try {
                auto j = nlohmann::json::parse(body);
                if (j.contains("model") && j["model"].is_string())
                    host.model_code = j["model"].get<std::string>();
                if (j.contains("mac") && j["mac"].is_string())
                    host.mac = j["mac"].get<std::string>();
                if (is_cfs_capable(host.model_code)) {
                    host.cfs_capable = true;
                    host.model_name  = model_name_for(host.model_code);
                }
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(warning)
                    << "CrealityHostDiscovery: /info parse failed for "
                    << host.ip << ": " << e.what();
            }
        })
        .on_error([&host](std::string /*body*/, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(info)
                << "CrealityHostDiscovery: /info GET failed for "
                << host.ip << ": " << error << " (HTTP " << status << ")";
        })
        .perform_sync();
}

} // namespace

std::string CrealityHostDiscovery::host_label(const std::string& service_name, const std::string& model_name)
{
    auto dash = service_name.find_last_of('-');
    if (dash == std::string::npos) return {};
    auto dot = service_name.find('.', dash);
    if (dot == std::string::npos) return {};
    std::string suffix = service_name.substr(dash + 1, dot - dash - 1);
    if (suffix.empty()) return {};
    if (suffix.size() > 4)
        suffix.erase(0, suffix.size() - 4);
    return (model_name.empty() ? "Creality" : model_name) + "-" + suffix;
}

std::vector<CrealityHost> CrealityHostDiscovery::scan(bool probe, int wait_seconds)
{
    // The device page scans in the background while the printer dialog may scan too, and cxmdns
    // keeps its buffers in statics
    static std::mutex scan_mutex;
    std::lock_guard<std::mutex> lock(scan_mutex);

    const std::vector<std::string> prefixes{ "Creality", "creality" };

    BOOST_LOG_TRIVIAL(info)
        << "CrealityHostDiscovery: starting DNS-SD discovery (prefixes: Creality, creality)";

    auto raw = cxnet::syncDiscoveryService(prefixes, wait_seconds);

    BOOST_LOG_TRIVIAL(info)
        << "CrealityHostDiscovery: mDNS returned " << raw.size() << " match(es)";

    std::vector<CrealityHost> hosts;
    hosts.reserve(raw.size());

    // Dedupe by IP -- one printer may announce twice if multi-homed or if
    // we capture both IPv4/IPv6 replies.
    std::vector<std::string> seen_ips;
    for (const auto& m : raw) {
        // A reply over IPv6 leaves only "[fe80" here, which is no address to use
        if (m.machineIp.empty() || m.machineIp.front() == '[') continue;
        if (std::find(seen_ips.begin(), seen_ips.end(), m.machineIp) != seen_ips.end())
            continue;
        seen_ips.push_back(m.machineIp);

        CrealityHost h;
        h.ip           = m.machineIp;
        h.service_name = m.answer;

        if (probe) {
            probe_info(h);
        }
        h.hostname = host_label(m.answer, h.model_name);

        hosts.push_back(std::move(h));
    }

    BOOST_LOG_TRIVIAL(info)
        << "CrealityHostDiscovery: " << hosts.size() << " unique host(s) after dedup";

    return hosts;
}

} // namespace Slic3r

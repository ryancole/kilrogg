#include "sender/displays.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

#include "common/log.h"

using Microsoft::WRL::ComPtr;

namespace krg {

namespace {

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    auto eq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), eq) != hay.end();
}

// Windows' own name for each monitor ("LG ULTRAGEAR"), keyed by the GDI device
// name DXGI reports for the output it is attached to. DXGI has no idea what a
// monitor is called, and EnumDisplayDevices mostly answers "Generic PnP
// Monitor", which does not tell two panels apart either — the display-config
// API is the only one that knows. Failure here costs nothing but the name:
// every display still lists and is still selectable by its \\.\DISPLAYn.
std::map<std::wstring, std::string> monitor_names() {
    std::map<std::wstring, std::string> out;
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) !=
        ERROR_SUCCESS) {
        return out;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                           modes.data(), nullptr) != ERROR_SUCCESS) {
        return out;
    }
    paths.resize(path_count);
    for (const DISPLAYCONFIG_PATH_INFO& p : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = p.sourceInfo.adapterId;
        src.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;

        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = p.targetInfo.adapterId;
        tgt.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;
        if (!tgt.monitorFriendlyDeviceName[0]) continue;

        out.emplace(src.viewGdiDeviceName, narrow(tgt.monitorFriendlyDeviceName));
    }
    return out;
}

bool is_primary(HMONITOR mon) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    return mon && GetMonitorInfoW(mon, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
}

} // namespace

std::string DisplayDevice::label() const {
    std::string s = monitor.empty() ? device : monitor + " (" + device + ")";
    if (!adapter.empty()) s += " on " + adapter;
    return s;
}

std::vector<DisplayDevice> list_displays() {
    std::vector<DisplayDevice> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        KRG_LOG("CreateDXGIFactory1 failed; no display could be enumerated");
        return out;
    }

    const std::map<std::wstring, std::string> names = monitor_names();
    for (UINT ai = 0;; ++ai) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(ai, &adapter))) break;
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        for (UINT oi = 0;; ++oi) {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(oi, &output))) break;
            DXGI_OUTPUT_DESC od{};
            // An output that is not attached to the desktop has nothing on it
            // to duplicate, so it is not a choice worth offering. This is also
            // what keeps the software adapters (Basic Render Driver, WARP) out
            // of the list without having to name them.
            if (FAILED(output->GetDesc(&od)) || !od.AttachedToDesktop) continue;

            DisplayDevice d;
            d.adapter_obj = adapter;
            d.output_obj = output;
            d.adapter = narrow(ad.Description);
            d.device = narrow(od.DeviceName);
            if (auto it = names.find(od.DeviceName); it != names.end()) d.monitor = it->second;
            d.x = od.DesktopCoordinates.left;
            d.y = od.DesktopCoordinates.top;
            d.width = static_cast<uint32_t>(od.DesktopCoordinates.right -
                                            od.DesktopCoordinates.left);
            d.height = static_cast<uint32_t>(od.DesktopCoordinates.bottom -
                                             od.DesktopCoordinates.top);
            d.primary = is_primary(od.Monitor);
            out.push_back(std::move(d));
        }
    }
    return out;
}

void print_displays(const std::vector<DisplayDevice>& displays) {
    if (displays.empty()) {
        KRG_LOG("no display is attached to this machine");
        return;
    }
    KRG_LOG("displays --display can name, by index or by any part of the text:");
    for (size_t i = 0; i < displays.size(); ++i) {
        const DisplayDevice& d = displays[i];
        KRG_LOG("  %zu  %ux%u at (%d,%d)%s  %s", i, d.width, d.height, d.x, d.y,
                d.primary ? "  [primary, the default]" : "", d.label().c_str());
    }
}

const DisplayDevice* select_display(const std::vector<DisplayDevice>& displays,
                                    const std::string& selector) {
    if (displays.empty()) {
        KRG_LOG("no display is attached to this machine, so there is nothing to capture");
        return nullptr;
    }
    if (selector.empty()) {
        for (const DisplayDevice& d : displays) {
            if (d.primary) return &d;
        }
        return &displays.front();
    }

    // All digits is an index into the listing above; anything else is a name.
    // A monitor called "4K" would be unreachable by name, but it is reachable
    // by index, and the reverse rule would make every index ambiguous.
    if (selector.find_first_not_of("0123456789") == std::string::npos) {
        const unsigned long i = std::strtoul(selector.c_str(), nullptr, 10);
        if (i < displays.size()) return &displays[i];
        KRG_LOG("--display %s: this machine has %zu, numbered 0 to %zu", selector.c_str(),
                displays.size(), displays.size() - 1);
        print_displays(displays);
        return nullptr;
    }

    const DisplayDevice* found = nullptr;
    size_t matches = 0;
    for (const DisplayDevice& d : displays) {
        if (!contains_ci(d.label(), selector)) continue;
        ++matches;
        if (!found) found = &d;
    }
    if (matches == 1) return found;
    if (matches == 0) {
        KRG_LOG("--display %s: no display's name contains that", selector.c_str());
    } else {
        KRG_LOG("--display %s: %zu displays' names contain that; name one of them or use its "
                "index", selector.c_str(), matches);
    }
    print_displays(displays);
    return nullptr;
}

} // namespace krg

#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <dxgi1_5.h>
#include <wrl/client.h>

namespace krg {

// One capturable display: a DXGI output together with the adapter it hangs
// off. The two travel as a pair because Desktop Duplication only works between
// an output and the adapter that owns it — which means choosing a display also
// chooses the GPU that capture, the BGRA->NV12 conversion and the encode all
// run on. Left to itself D3D11 hands back whatever adapter Windows lists
// first, so on a hybrid machine that was the weaker GPU as often as not, and
// nothing said which one it had been.
struct DisplayDevice {
    // Kept alive so the capture that follows a selection uses the very objects
    // that were enumerated, rather than re-walking DXGI and trusting the
    // indices to have stayed put.
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_obj;
    Microsoft::WRL::ComPtr<IDXGIOutput> output_obj;

    std::string adapter; // "NVIDIA GeForce RTX 4090"
    std::string device;  // "\\.\DISPLAY1" — the name EnumDisplaySettings takes
    std::string monitor; // "LG ULTRAGEAR", empty where Windows has no name for it
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
    bool primary = false;

    // The one line that identifies this display, and the text --display
    // matches a name against.
    std::string label() const;
};

// Every output currently attached to the desktop, adapter by adapter, in the
// order --display numbers them from 0. Empty when DXGI cannot be reached or
// nothing is attached, which is a machine with no desktop to duplicate.
std::vector<DisplayDevice> list_displays();

// Resolves what the operator asked for: an index into the list above, or a
// case-insensitive substring of a display's label. An empty selector picks the
// primary, which is where capture has always pointed. Returns null and says
// why if nothing matches, or if a name matches more than one display —
// silently capturing a different screen than the one that was named is worse
// than refusing to start.
const DisplayDevice* select_display(const std::vector<DisplayDevice>& displays,
                                    const std::string& selector);

// Writes the listing --list-displays prints.
void print_displays(const std::vector<DisplayDevice>& displays);

} // namespace krg

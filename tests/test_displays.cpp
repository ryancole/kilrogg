#include <doctest/doctest.h>

#include <string>

#include "sender/displays.h"

using namespace krg;

namespace {

// A display with no DXGI objects behind it. Everything --display resolves
// against is text and a primary flag, so the ComPtrs stay null: the selection
// rules are what is under test, not the enumeration that fills them in.
DisplayDevice display(std::string device, std::string monitor, std::string adapter,
                      bool primary = false) {
    DisplayDevice d;
    d.device = std::move(device);
    d.monitor = std::move(monitor);
    d.adapter = std::move(adapter);
    d.primary = primary;
    return d;
}

DisplayList two_screens() {
    DisplayList list;
    list.displays.push_back(
        display("\\\\.\\DISPLAY1", "Dell AW3423DW", "NVIDIA GeForce RTX 4090", true));
    list.displays.push_back(display("\\\\.\\DISPLAY2", "LG ULTRAGEAR", "Intel UHD Graphics 770"));
    return list;
}

} // namespace

TEST_CASE("a display's label names the monitor, the device and the GPU") {
    CHECK(display("\\\\.\\DISPLAY1", "Dell AW3423DW", "NVIDIA GeForce RTX 4090").label() ==
          "Dell AW3423DW (\\\\.\\DISPLAY1) on NVIDIA GeForce RTX 4090");
    // Windows has no friendly name for every panel; the device name stands in
    // rather than leaving an empty pair of brackets.
    CHECK(display("\\\\.\\DISPLAY2", "", "Intel UHD Graphics 770").label() ==
          "\\\\.\\DISPLAY2 on Intel UHD Graphics 770");
}

TEST_CASE("no selector picks the primary, wherever it sits in the list") {
    DisplayList list = two_screens();
    REQUIRE(select_display(list, "") == &list.displays[0]);

    // The primary is not always enumerated first.
    list.displays[0].primary = false;
    list.displays[1].primary = true;
    CHECK(select_display(list, "") == &list.displays[1]);
}

TEST_CASE("a list with no primary flagged falls back to the first display") {
    DisplayList list = two_screens();
    list.displays[0].primary = false;
    CHECK(select_display(list, "") == &list.displays[0]);
}

TEST_CASE("an all-digits selector is an index into the listing") {
    DisplayList list = two_screens();
    CHECK(select_display(list, "0") == &list.displays[0]);
    CHECK(select_display(list, "1") == &list.displays[1]);
}

TEST_CASE("an index past the end stops the sender rather than falling back") {
    DisplayList list = two_screens();
    // Capturing a screen other than the one asked for is not an improvement on
    // saying so.
    CHECK(select_display(list, "2") == nullptr);
    CHECK(select_display(list, "99") == nullptr);
}

TEST_CASE("a name matches any part of the label, case-insensitively") {
    DisplayList list = two_screens();
    CHECK(select_display(list, "ultragear") == &list.displays[1]);
    CHECK(select_display(list, "ULTRAGEAR") == &list.displays[1]);
    CHECK(select_display(list, "AW3423") == &list.displays[0]);
    // The GPU is part of the label too, which is how one card's screen is
    // named on a hybrid machine.
    CHECK(select_display(list, "intel") == &list.displays[1]);
    CHECK(select_display(list, "DISPLAY2") == &list.displays[1]);
}

TEST_CASE("a name matching more than one display refuses to guess") {
    DisplayList list;
    list.displays.push_back(display("\\\\.\\DISPLAY1", "LG ULTRAGEAR", "NVIDIA GeForce RTX 4090"));
    list.displays.push_back(display("\\\\.\\DISPLAY2", "LG ULTRAFINE", "NVIDIA GeForce RTX 4090"));
    CHECK(select_display(list, "ultra") == nullptr);
    CHECK(select_display(list, "NVIDIA") == nullptr);
    // Narrowed until it is unambiguous, it resolves.
    CHECK(select_display(list, "ultrafine") == &list.displays[1]);
    // And the index remains available when no name can separate them.
    CHECK(select_display(list, "0") == &list.displays[0]);
}

TEST_CASE("a name matching nothing refuses rather than falling back to the primary") {
    DisplayList list = two_screens();
    CHECK(select_display(list, "samsung") == nullptr);
}

TEST_CASE("a machine with no attached display has nothing to select") {
    DisplayList empty;
    CHECK(select_display(empty, "") == nullptr);
    CHECK(select_display(empty, "0") == nullptr);
    CHECK(select_display(empty, "anything") == nullptr);
}

TEST_CASE("a monitor named in digits is reachable by index even though not by name") {
    // The documented trade: all-digits is an index, so a panel called "4K"
    // cannot be named — but the reverse rule would make every index ambiguous.
    DisplayList list;
    list.displays.push_back(display("\\\\.\\DISPLAY1", "4K", "NVIDIA GeForce RTX 4090", true));
    CHECK(select_display(list, "0") == &list.displays[0]);
    CHECK(select_display(list, "4K") == &list.displays[0]); // not all digits, so a name
}

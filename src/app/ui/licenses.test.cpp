#include "licenses.hpp"
#include <cassert>
#include <exception>
#include <iostream>

int main() try {
    using namespace voyemu::ui;
    const std::string full = "Автор ©\n" + std::string(200000, 'x') + "\nEND OF COMPLETE NOTICE\n";
    const LicenseNotice fixture{"Component", "Authors © 2026", "MIT",
                                "https://example.invalid/license", full};
    assert(license_document(fixture) == full);
    const auto entries = embedded_license_notices();
    assert(entries.size() >= 6);
    assert(entries[0].component == "Voyemu");
    bool json = false, imgui = false;

    for (const auto &entry : entries) {
        assert(!entry.component.empty() && !entry.authors.empty() && !entry.license.empty() &&
               !entry.text.empty());
        assert(entry.url.starts_with("https://"));
        assert(entry.component.find("macOS") == std::string_view::npos &&
               entry.component.find("Windows") == std::string_view::npos);

        if (entry.component != "Voyemu") {
            for (const auto language : {Language::russian, Language::english, Language::chinese}) {
                assert(license_site(entry, language) == entry.url);
            }
        }

        assert(license_document(entry) == entry.text);
        assert(entry.text.find('\r') == std::string_view::npos);
        assert(entry.component != "QEMU" && entry.component != "JDK" && entry.component != "LZO");
        json |= entry.component == "nlohmann/json";
        imgui |= entry.component == "Dear ImGui";
    }

    assert(json && imgui);
    assert(entries[4].component == "lzokay" && entries[5].component == "miniz");
    assert(license_site(entries[0], Language::russian) == "https://voyemu.ru");
    assert(license_site(entries[0], Language::english) == "https://voyemu.com");
    assert(license_site(entries[0], Language::chinese) == "https://voyemu.com");
#ifdef __APPLE__
    assert(entries.size() == 10);
    assert(entries[6].component == "libarchive" && entries[7].component == "nghttp2" &&
           entries[8].component == "Android Debug Bridge (AOSP)" && entries[9].component == "SDL3");
#else
    assert(entries.size() == 10);
    assert(entries[6].component == "curl" && entries[7].component == "nghttp2" &&
           entries[8].component == "Android Debug Bridge (AOSP)" && entries[9].component == "SDL3");
#endif
    // SDL3 carries the notices of the third-party code it compiles into the static library.
    const auto &sdl = entries[9].text;

    for (const auto *author : {"Sam Lantinga", "Adrien Descamps", "Sun Microsystems",
                               "Gareth McCaughan", "Andreas Schiffler", "Valve Corporation"}) {
        assert(sdl.find(author) != std::string_view::npos);
    }

#ifdef __linux__
    assert(sdl.find("xdg-user-dirs") != std::string_view::npos &&
           sdl.find("Copyright (c) 2007 Red Hat, Inc.") != std::string_view::npos);

    // The X11 and Wayland drivers: EDID and XSETTINGS parsing, the keysym table and the protocols.
    for (const auto *author : {"Owen Taylor", "Igalia S.L.", "Jamey Sharp", "Kristian Høgsberg",
                               "Jonas Ådahl", "Simon Ser"}) {
        assert(sdl.find(author) != std::string_view::npos);
    }
#else
    assert(sdl.find("xdg-user-dirs") == std::string_view::npos &&
           sdl.find("Igalia S.L.") == std::string_view::npos);
#endif
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}

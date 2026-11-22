#include "portable_fonts.hpp"
#include "process.hpp"
#include <fstream>
#include <iostream>
#include <memory>
#include <miniz.h>
using namespace voyemu;

namespace {
void check(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::string failure(const std::function<void()> &action) {
    try {
        action();
    } catch (const std::exception &error) {
        return error.what();
    }

    return {};
}

void put(const std::filesystem::path &file, const std::string &bytes) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

std::string read(const std::filesystem::path &file) {
    std::ifstream input(file, std::ios::binary);

    return {std::istreambuf_iterator<char>(input), {}};
}

std::string apk(const std::string &entry, const std::string &bytes) {
    mz_zip_archive archive{};

    check(mz_zip_writer_init_heap(&archive, 0, 0), "APK fixture initialization");
    check(mz_zip_writer_add_mem(&archive, entry.c_str(), bytes.data(), bytes.size(),
                                MZ_DEFAULT_COMPRESSION),
          "APK fixture entry");
    void *data = nullptr;
    std::size_t size = 0;
    check(mz_zip_writer_finalize_heap_archive(&archive, &data, &size), "APK fixture finalization");
    const std::unique_ptr<void, decltype(&mz_free)> owned(data, mz_free);
    mz_zip_writer_end(&archive);

    return {static_cast<const char *>(owned.get()), size};
}
} // namespace

int main(int argc, char **argv) try {
    check(argc == 3, "usage: portable_fonts_test FIXTURE_FONTS SCRATCH");
    const std::filesystem::path fixtures = argv[1];
    const auto root = std::filesystem::absolute(argv[2]) / ("portable-fonts-" + random_token());
    const std::string profile = "synthetic-fonts";
    const auto originals = root / ".local/profiles" / profile / "originals";
    const auto latin = read(fixtures / "primary-regular.ttf"),
               bold = read(fixtures / "primary-bold.ttf"),
               chinese = read(fixtures / "fallback-regular.ttf");
    const auto package = apk("res/font/regular.ttf", chinese);
    put(originals / "fonts/latin-regular.ttf", latin);
    put(originals / "fonts/latin-bold.ttf", bold);
    put(originals / "App.apk", package);
    const auto original = [](const std::string &file, const std::string &bytes) {
        return Json{{"file", file},
                    {"bytes", bytes.size()},
                    {"sha256", sha256_bytes(bytes)},
                    {"source", {{"path", "/system/" + file}}},
                    {"acquisition", {{"image", "system"}, {"path", "/system/" + file}}}};
    };
    const Json entry = {{"original", "App"},
                        {"entry", "res/font/regular.ttf"},
                        {"bytes", chinese.size()},
                        {"sha256", sha256_bytes(chinese)}};
    Json definition = {{"original",
                        {{"Regular", original("fonts/latin-regular.ttf", latin)},
                         {"Bold", original("fonts/latin-bold.ttf", bold)},
                         {"App", original("App.apk", package)}}},
                       {"fonts",
                        {{"primary",
                          {{"regular", {{"original", "Regular"}}},
                           {"medium", {{"original", "Bold"}}},
                           {"bold", {{"original", "Bold"}}}}},
                         {"fallback", {{"regular", entry}, {"medium", entry}, {"bold", entry}}}}}};
    const auto seed = root / "machine";
    std::filesystem::create_directories(seed / "ui");
    const auto resources = export_portable_fonts(root, definition, profile, seed);
    check(resources.size() == 6, "six faces of a machine");

    for (const auto &[name, expected] : {std::pair{"ui/fonts/primary-regular.ttf", &latin},
                                         std::pair{"ui/fonts/primary-medium.ttf", &bold},
                                         std::pair{"ui/fonts/primary-bold.ttf", &bold},
                                         std::pair{"ui/fonts/fallback-regular.ttf", &chinese},
                                         std::pair{"ui/fonts/fallback-bold.ttf", &chinese}}) {
        check(read(seed / name) == *expected &&
                      resources.at(name) == Json{{"file", name},
                                                 {"bytes", expected->size()},
                                                 {"sha256", sha256_bytes(*expected)}},
              "a declared original or APK entry becomes the face of its weight");
    }

    check(!std::filesystem::exists(seed / "ui/fonts/.stage"), "no staged font remains");
    check(export_portable_fonts(root, Json{{"original", definition.at("original")}}, profile,
                                root / "other")
                  .empty(),
          "a profile without fonts exports none");

    auto changed = definition;
    changed["fonts"]["fallback"]["bold"]["sha256"] = std::string(64, 'a');
    std::filesystem::remove_all(seed / "ui/fonts");
    check(!failure([&] {
               (void)export_portable_fonts(root, changed, profile, seed);
           }).empty(),
          "an APK entry that differs from its pin stops the build");

    changed = definition;
    changed["fonts"]["primary"].erase("medium");
    check(failure([&] {
              (void)export_portable_fonts(root, changed, profile, seed);
          }).starts_with("PORTABLE_FONTS: "),
          "every weight of each role is required");

    put(originals / "fonts/latin-bold.ttf", latin);
    check(failure([&] {
              (void)export_portable_fonts(root, definition, profile, seed);
          }).starts_with("PORTABLE_FONTS: "),
          "an imported original that differs from its pin stops the build");

    std::filesystem::remove_all(root);
    std::cout << "portable fonts: pass\n";

    return 0;
} catch (const std::exception &error) {
    std::cerr << "portable fonts: " << error.what() << '\n';

    return 1;
}

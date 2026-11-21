#include "model.hpp"

namespace voyemu::detail {
namespace {
struct PassionSignalRow {
    std::string_view name;
    unsigned width;
    std::uint32_t initial;
    SignalDefinition signal;
};
#include "passion_catalog.inc"
} // namespace

// The 1598 cluster input signals of the Passion SOME/IP service table in table order, numbered
// from 1; SignalDefinition::id keeps the signal ID of the table.
const std::vector<Field> &passion_catalog() {
    static const auto fields = [] {
        std::vector<Field> result;
        std::uint32_t index = 0;

        for (const auto &row : passion_rows) {
            result.push_back({++index, std::string(row.name), row.width, row.initial});
        }

        return result;
    }();

    return fields;
}

std::span<const SignalDefinition> passion_signals() {
    static const auto signals = [] {
        std::vector<SignalDefinition> result;

        for (const auto &row : passion_rows) {
            result.push_back(row.signal);
        }

        return result;
    }();

    return signals;
}
} // namespace voyemu::detail

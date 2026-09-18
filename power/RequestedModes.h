// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <charconv>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace marble::power {

// One boot-local property, independent of asynchronous applied-hint properties.
// Version/bit assignments are a restart contract: do not reorder these modes.
inline constexpr char kRequestedModesProperty[] = "vendor.powerhal.marble.requested";
inline constexpr std::array<std::string_view, 6> kSavedModes = {
        "INTERACTIVE", "LOW_POWER", "SUSTAINED_PERFORMANCE", "FIXED_PERFORMANCE",
        "EXPENSIVE_RENDERING", "Flipendo"};

struct RequestedModes {
    std::set<std::string> modes;
    bool interactiveKnown = false;
};

inline std::string encodeRequestedModes(const std::set<std::string>& modes,
                                         bool interactiveKnown) {
    unsigned bits = interactiveKnown ? 1u : 0u;
    for (size_t i = 0; i < kSavedModes.size(); ++i) {
        if (modes.contains(std::string(kSavedModes[i]))) bits |= 1u << (i + 1);
    }
    return "v1:" + std::to_string(bits);
}

inline std::optional<RequestedModes> decodeRequestedModes(std::string_view value) {
    // A fresh boot (or upgrade from the old applied-property scheme) has no
    // snapshot. Never infer hidden intent from applied properties as a fallback.
    if (value.empty()) return RequestedModes{};
    if (!value.starts_with("v1:")) return std::nullopt;
    value.remove_prefix(3);
    unsigned bits = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), bits);
    if (error != std::errc{} || end != value.data() + value.size() || bits > 127u ||
        ((bits & 2u) && !(bits & 1u))) return std::nullopt;
    RequestedModes state;
    state.interactiveKnown = bits & 1u;
    for (size_t i = 0; i < kSavedModes.size(); ++i) {
        if (bits & (1u << (i + 1))) state.modes.emplace(kSavedModes[i]);
    }
    return state;
}

// Property I/O is supplied by the service; host tests use an in-memory store.
// The store must outlive the controller. Calls are serialized by its mutex.
class RequestedModeStore {
  public:
    virtual ~RequestedModeStore() = default;
    virtual std::string load() const = 0;
    virtual bool save(const std::string& value) = 0;
};

}  // namespace marble::power

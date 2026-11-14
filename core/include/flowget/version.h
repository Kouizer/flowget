#pragma once

#include <flowget/version_generated.h>

#include <cstdint>
#include <string_view>

namespace flowget {

inline constexpr std::string_view kVersionString = FLOWGET_VERSION_STRING;
inline constexpr std::uint32_t kVersionMajor = FLOWGET_VERSION_MAJOR;
inline constexpr std::uint32_t kVersionMinor = FLOWGET_VERSION_MINOR;
inline constexpr std::uint32_t kVersionPatch = FLOWGET_VERSION_PATCH;

} // namespace flowget

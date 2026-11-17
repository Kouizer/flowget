#include <flowget/flowget.h>

#include <array>
#include <cstddef>
#include <string_view>

namespace {

// Indexed by flowget_status; order must match the enum in flowget.h.
constexpr std::array<std::string_view, 13> kStatusNames = {
    "ok",
    "invalid_argument",
    "out_of_memory",
    "not_found",
    "busy",
    "unsupported",
    "cancelled",
    "io",
    "network",
    "range_not_supported",
    "checksum_mismatch",
    "internal",
    "unknown",
};

static_assert(kStatusNames.size() == static_cast<std::size_t>(FLOWGET_STATUS_UNKNOWN) + 1,
              "every flowget_status needs a name");

} // namespace

extern "C" {

uint32_t flowget_api_version(void) {
    return FLOWGET_API_VERSION;
}

const char* flowget_status_name(flowget_status status) {
    const auto index = static_cast<std::size_t>(status);
    // The bound is checked on this line and `back()` covers every out-of-range value; the
    // alternative, `.at()`, would throw across the C ABI that promises it never does.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return index < kStatusNames.size() ? kStatusNames[index].data() : kStatusNames.back().data();
}

} // extern "C"

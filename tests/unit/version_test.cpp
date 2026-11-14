#include <flowget/flowget.h>
#include <flowget/version.h>

#include <cstring>
#include <string_view>

#include <gtest/gtest.h>

namespace {

TEST(Version, AbiVersionIsStable) {
    EXPECT_EQ(flowget_api_version(), FLOWGET_API_VERSION);
    EXPECT_EQ(FLOWGET_API_VERSION, 1u);
}

TEST(Version, VersionStringMatchesBuildMetadata) {
    const std::string_view reported{flowget_version_string()};
    EXPECT_EQ(reported, flowget::kVersionString);
    EXPECT_FALSE(reported.empty());
}

TEST(Version, StatusNamesAreTotal) {
    EXPECT_STREQ(flowget_status_name(FLOWGET_STATUS_OK), "ok");
    EXPECT_STREQ(flowget_status_name(FLOWGET_STATUS_RANGE_NOT_SUPPORTED), "range_not_supported");
    EXPECT_STREQ(flowget_status_name(FLOWGET_STATUS_UNKNOWN), "unknown");

    // Out-of-range input must never crash or return nullptr.
    const auto bogus = static_cast<flowget_status>(999);
    ASSERT_NE(flowget_status_name(bogus), nullptr);
    EXPECT_STREQ(flowget_status_name(bogus), "unknown");
}

} // namespace

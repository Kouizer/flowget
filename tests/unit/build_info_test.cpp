#include <flowget/build_info.h>

#include <string>

#include <gtest/gtest.h>

// These tests exist to prove that the four vcpkg ports are *linked*, not merely installed: if a
// library were missing or the triplet were wrong, linking would fail or the reported version would
// be empty. Version numbers are not pinned — the manifest baseline is (vcpkg.json).
namespace {

TEST(BuildInfo, EveryDependencyReportsAVersion) {
    const flowget::DependencyVersions versions = flowget::dependency_versions();

    EXPECT_FALSE(versions.curl.empty());
    EXPECT_FALSE(versions.openssl.empty());
    EXPECT_FALSE(versions.sqlite.empty());
    EXPECT_FALSE(versions.spdlog.empty());
}

TEST(BuildInfo, CurlReportsVersionString) {
    const flowget::DependencyVersions versions = flowget::dependency_versions();
    EXPECT_NE(versions.curl.find_first_of("0123456789"), std::string::npos)
        << "curl version string has no digit: " << versions.curl;
}

TEST(BuildInfo, OpenSslAndSqliteReportExpectedFamily) {
    const flowget::DependencyVersions versions = flowget::dependency_versions();
    EXPECT_NE(versions.openssl.find("OpenSSL"), std::string::npos) << versions.openssl;
    EXPECT_EQ(versions.sqlite.rfind("3.", 0), 0u) << versions.sqlite;
}

TEST(BuildInfo, SummaryMentionsEveryLibrary) {
    const std::string summary = flowget::dependency_summary();
    EXPECT_NE(summary.find("curl "), std::string::npos) << summary;
    EXPECT_NE(summary.find("OpenSSL "), std::string::npos) << summary;
    EXPECT_NE(summary.find("SQLite "), std::string::npos) << summary;
    EXPECT_NE(summary.find("spdlog "), std::string::npos) << summary;
}

} // namespace

#include <flowget/build_info.h>

#include <curl/curl.h>
#include <openssl/crypto.h>
#include <spdlog/version.h>
#include <sqlite3.h>

namespace flowget {

DependencyVersions dependency_versions() {
    const curl_version_info_data* curl_info = curl_version_info(CURLVERSION_NOW);

    return DependencyVersions{
        .curl =
            curl_info != nullptr && curl_info->version != nullptr ? curl_info->version : "unknown",
        .openssl = OpenSSL_version(OPENSSL_VERSION),
        .sqlite = sqlite3_libversion(),
        .spdlog = std::to_string(SPDLOG_VER_MAJOR) + "." + std::to_string(SPDLOG_VER_MINOR) + "." +
                  std::to_string(SPDLOG_VER_PATCH),
    };
}

std::string dependency_summary() {
    const DependencyVersions versions = dependency_versions();
    // `OpenSSL_version()` already returns a string that starts with "OpenSSL", so it is not
    // prefixed again here.
    return "curl " + versions.curl + ", " + versions.openssl + ", SQLite " + versions.sqlite +
           ", spdlog " + versions.spdlog;
}

} // namespace flowget

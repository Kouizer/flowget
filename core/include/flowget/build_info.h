#ifndef FLOWGET_BUILD_INFO_H
#define FLOWGET_BUILD_INFO_H

#include <string>

namespace flowget {

// Versions of the linked third-party libraries, as reported by the libraries themselves. Used by
// `flowget-cli --version`, in bug reports, and by a unit test that proves all four ports are
// actually linked into the core (not just installed next to it).
struct DependencyVersions {
    std::string curl;
    std::string openssl;
    std::string sqlite;
    std::string spdlog;
};

DependencyVersions dependency_versions();

// One-line summary: "curl 8.17.0, OpenSSL 3.5.4, SQLite 3.51.0, spdlog 1.15.3".
std::string dependency_summary();

} // namespace flowget

#endif /* FLOWGET_BUILD_INFO_H */

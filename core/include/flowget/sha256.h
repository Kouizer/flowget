#ifndef FLOWGET_SHA256_H
#define FLOWGET_SHA256_H

#include <cstddef>
#include <memory>
#include <string>

namespace flowget {

// Streaming SHA-256 built on OpenSSL EVP. It lives in core (not tests/support) because the S8
// HashVerifier will wrap it; the segmented engine and the CLI both need the same implementation.
//
// The class is move-only and has no error exceptions: an allocation failure or a libcrypto error is
// reported by `ok()` and by an empty digest, never by throwing across a boundary (ADR-0002).
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(Sha256&&) noexcept;
    Sha256& operator=(Sha256&&) noexcept;
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    // False after a construction failure or a failed update/digest.
    [[nodiscard]] bool ok() const noexcept;

    // Feeds `size` bytes. A null pointer with a non-zero size is a caller bug and fails the hasher;
    // calling update() after hex_digest() is a no-op.
    void update(const void* data, std::size_t size);

    // Finalises the hash and returns 64 lowercase hex characters. Empty when the hasher failed or
    // when it was already finalised (a digest cannot be extended).
    std::string hex_digest();

    // One-shot convenience: constructs a hasher, feeds the whole buffer and finalises it.
    static std::string hex_digest(const void* data, std::size_t size);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace flowget

#endif /* FLOWGET_SHA256_H */

#include <flowget/sha256.h>

#include <array>
#include <cstdint>
#include <new>
#include <string_view>
#include <utility>

#include <openssl/evp.h>

namespace flowget {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";

// SHA-256 always produces 32 bytes; EVP_MAX_MD_SIZE leaves room for larger digests.
constexpr unsigned int kSha256DigestLength = 32;

} // namespace

struct Sha256::Impl {
    EVP_MD_CTX* context;
    bool finished = false;
    bool failed = false;

    Impl() : context(EVP_MD_CTX_new()) {
        if (context == nullptr) {
            failed = true;
            return;
        }
        if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
            failed = true;
        }
    }

    ~Impl() {
        if (context != nullptr) {
            EVP_MD_CTX_free(context);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
};

Sha256::Sha256() {
    // nothrow: the constructor is not noexcept in the header, but the contract says a failure is
    // reported through ok() rather than an exception crossing the C ABI boundary (ADR-0002).
    impl_.reset(new (std::nothrow) Impl());
}

Sha256::~Sha256() = default;

Sha256::Sha256(Sha256&&) noexcept = default;

Sha256& Sha256::operator=(Sha256&&) noexcept = default;

bool Sha256::ok() const noexcept {
    return impl_ != nullptr && !impl_->failed;
}

void Sha256::update(const void* data, std::size_t size) {
    if (impl_ == nullptr || impl_->failed || impl_->finished) {
        return;
    }
    if (size == 0) {
        return;
    }
    if (data == nullptr) {
        // A null buffer with a non-zero size is a caller bug, not an empty update.
        impl_->failed = true;
        return;
    }
    if (EVP_DigestUpdate(impl_->context, data, size) != 1) {
        impl_->failed = true;
    }
}

std::string Sha256::hex_digest() {
    if (impl_ == nullptr || impl_->failed || impl_->finished) {
        return {};
    }
    impl_->finished = true;

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(impl_->context, digest.data(), &length) != 1 ||
        length != kSha256DigestLength) {
        impl_->failed = true;
        return {};
    }

    std::string result;
    result.reserve(static_cast<std::size_t>(length) * 2);
    for (unsigned int index = 0; index < length; ++index) {
        // `index` is bounded by `length`, the digest size EVP just reported, so this subscript
        // cannot escape the array; `.at()` would add a throw path for no reachable case.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): bounded by length
        const auto byte = static_cast<unsigned int>(digest[index]);
        result.push_back(kHexDigits[(byte >> 4) & 0x0FU]);
        result.push_back(kHexDigits[byte & 0x0FU]);
    }
    return result;
}

std::string Sha256::hex_digest(const void* data, std::size_t size) {
    Sha256 hasher;
    hasher.update(data, size);
    return hasher.hex_digest();
}

} // namespace flowget

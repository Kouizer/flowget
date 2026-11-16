#include <flowget/sha256.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using flowget::Sha256;

// Lowercase hex is part of the public contract: the S8 HashVerifier compares user-supplied digests
// case-insensitively, but it normalises against these strings first.
constexpr const char* kEmptyDigest =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr const char* kAbcDigest =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr const char* kLongDigest =
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
constexpr const char* kMillionADigest =
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

std::string digest_of(const std::string& text) {
    return Sha256::hex_digest(text.data(), text.size());
}

TEST(Sha256Test, EmptyInputMatchesKnownVector) {
    EXPECT_EQ(digest_of(""), kEmptyDigest);
}

TEST(Sha256Test, AbcMatchesKnownVector) {
    EXPECT_EQ(digest_of("abc"), kAbcDigest);
}

TEST(Sha256Test, LongMessageMatchesKnownVector) {
    // NIST FIPS 180-2 sample: 448 bits, i.e. two padded blocks.
    const std::string message = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    EXPECT_EQ(digest_of(message), kLongDigest);
}

TEST(Sha256Test, MillionAsMatchesKnownVector) {
    const std::string message(1000U * 1000U, 'a');
    EXPECT_EQ(digest_of(message), kMillionADigest);
}

TEST(Sha256Test, IncrementalUpdatesMatchOneShot) {
    const std::string message =
        "The quick brown fox jumps over the lazy dog, then keeps running for a while.";
    const std::string expected = digest_of(message);

    Sha256 hasher;
    ASSERT_TRUE(hasher.ok());
    // Sizes deliberately cut across the 64-byte block boundary and include an empty update.
    hasher.update(message.data(), 7);
    hasher.update(nullptr, 0);
    hasher.update(message.data() + 7, 64);
    hasher.update(message.data() + 71, message.size() - 71);
    EXPECT_EQ(hasher.hex_digest(), expected);
}

TEST(Sha256Test, DigestCannotBeExtendedAndIsReturnedOnlyOnce) {
    Sha256 hasher;
    ASSERT_TRUE(hasher.ok());
    hasher.update("abc", 3);
    EXPECT_EQ(hasher.hex_digest(), kAbcDigest);

    // A finalised context does not accept more data and cannot produce a second digest.
    hasher.update("more", 4);
    EXPECT_EQ(hasher.hex_digest(), std::string{});
}

TEST(Sha256Test, NullBufferWithNonZeroSizeFailsTheHasher) {
    Sha256 hasher;
    ASSERT_TRUE(hasher.ok());
    hasher.update(nullptr, 1);
    EXPECT_FALSE(hasher.ok());
    EXPECT_EQ(hasher.hex_digest(), std::string{});
}

TEST(Sha256Test, MovedFromHasherIsEmptyAndMovedToHashes) {
    Sha256 source;
    ASSERT_TRUE(source.ok());
    source.update("abc", 3);

    Sha256 moved(std::move(source));
    EXPECT_TRUE(moved.ok());
    EXPECT_FALSE(source.ok());
    EXPECT_EQ(source.hex_digest(), std::string{});
    EXPECT_EQ(moved.hex_digest(), kAbcDigest);
}

TEST(Sha256Test, BinaryBytesWithHighBitAreHashed) {
    // Regression guard against signed-char handling: the digest must consume the bytes as unsigned.
    const std::vector<std::byte> data{
        std::byte{0x00}, std::byte{0x80}, std::byte{0xFF}, std::byte{0x7F}};
    const std::string digest = Sha256::hex_digest(data.data(), data.size());
    ASSERT_EQ(digest.size(), 64U);

    Sha256 hasher;
    hasher.update(data.data(), data.size());
    EXPECT_EQ(hasher.hex_digest(), digest);

    // Every hex character must be lowercase; an uppercase digest would break exact comparisons in
    // S8.
    for (const char character : digest) {
        EXPECT_TRUE((character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f'))
            << "unexpected character: " << character;
    }
}

} // namespace

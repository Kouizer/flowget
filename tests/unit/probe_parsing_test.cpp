#include <flowget/http_filename.h>
#include <flowget/http_range.h>

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using flowget::filename_from_content_disposition;
using flowget::filename_from_url;
using flowget::parse_content_range;

TEST(ContentRange, ParsesSatisfiedRange) {
    const auto range = parse_content_range("bytes 0-0/12345");
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(range->first, 0);
    EXPECT_EQ(range->last, 0);
    EXPECT_EQ(range->total, 12345);
    EXPECT_TRUE(range->has_range());
}

TEST(ContentRange, ParsesUnknownTotal) {
    // RFC 9110 allows `*` as the complete length; the parser reports -1 instead of guessing.
    const auto range = parse_content_range("bytes 100-199/*");
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(range->first, 100);
    EXPECT_EQ(range->last, 199);
    EXPECT_EQ(range->total, -1);
    EXPECT_TRUE(range->has_range());
}

TEST(ContentRange, ParsesUnsatisfiableRange) {
    // The 416 body of a zero-byte file: useful only for its total.
    const auto range = parse_content_range("bytes */0");
    ASSERT_TRUE(range.has_value());
    EXPECT_FALSE(range->has_range());
    EXPECT_EQ(range->total, 0);
}

TEST(ContentRange, ToleratesWhitespaceAndUnitCase) {
    const auto range = parse_content_range("  BYTES 0-99 / 1000  ");
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(range->first, 0);
    EXPECT_EQ(range->last, 99);
    EXPECT_EQ(range->total, 1000);
}

TEST(ContentRange, RejectsMalformedValues) {
    const std::vector<std::string> malformed = {
        "",
        "bytes",
        "bytes 0",
        "bytes 0-0",
        "bytes 0-0/",
        "bytes */",
        "bytes */*",
        "bytes -1/10",
        "bytes 0-/10",
        "bytes 5-1/10",                   // inverted range
        "bytes 0-10/10",                  // last byte outside the announced total
        "bytes 0-0/0",                    // an empty representation cannot satisfy 0-0
        "bytes 0-0/99999999999999999999", // total overflows int64
        "items 0-0/10",                   // unknown unit
        "bytes 0x0-0x1/10",               // non-decimal digits
        "bytes0-0/10",                    // missing separator after the unit
    };
    for (const std::string& value : malformed) {
        EXPECT_FALSE(parse_content_range(value).has_value()) << "accepted: " << value;
    }
}

TEST(Filename, UsesQuotedFilenameParameter) {
    EXPECT_EQ(filename_from_content_disposition("attachment; filename=\"report.pdf\""),
              "report.pdf");
}

TEST(Filename, UsesUnquotedFilenameParameter) {
    EXPECT_EQ(filename_from_content_disposition("attachment; filename=report.pdf"), "report.pdf");
}

TEST(Filename, PrefersRfc5987ExtendedParameter) {
    EXPECT_EQ(filename_from_content_disposition(
                  "attachment; filename=\"fallback.bin\"; filename*=UTF-8''na%C3%AFve%20file.txt"),
              "naïve file.txt");
}

TEST(Filename, AcceptsLanguageInExtendedParameter) {
    EXPECT_EQ(filename_from_content_disposition("attachment; filename*=UTF-8'en'caf%C3%A9.txt"),
              "café.txt");
}

TEST(Filename, IgnoresNonUtf8ExtendedParameter) {
    EXPECT_EQ(filename_from_content_disposition("attachment; filename*=iso-8859-1''caf%E9.txt"),
              "");
}

TEST(Filename, StripsDirectoryComponents) {
    EXPECT_EQ(filename_from_content_disposition("attachment; filename=\"../../etc/passwd\""),
              "passwd");
    EXPECT_EQ(filename_from_content_disposition("attachment; filename=\"C:\\tmp\\evil.exe\""),
              "evil.exe");
}

TEST(Filename, RemovesCharactersWindowsRejects) {
    EXPECT_EQ(filename_from_content_disposition("attachment; filename=\"a:b*c?.txt\""), "abc.txt");
}

TEST(Filename, DropsControlCharacters) {
    EXPECT_EQ(filename_from_content_disposition("attachment; filename=\"a\nb.bin\""), "ab.bin");
}

TEST(Filename, TrimsTrailingDotsAndSpaces) {
    EXPECT_EQ(filename_from_content_disposition("attachment; filename=\"name. \""), "name");
}

TEST(Filename, EmptyWhenNoUsableParameter) {
    EXPECT_EQ(filename_from_content_disposition("inline"), "");
    EXPECT_EQ(filename_from_content_disposition("attachment; filename=\"..\""), "");
    EXPECT_EQ(filename_from_content_disposition("attachment; filename=\"\""), "");
}

TEST(FilenameFromUrl, UsesLastPathSegment) {
    EXPECT_EQ(filename_from_url("https://example.com/dir/file.bin?x=1#fragment"), "file.bin");
}

TEST(FilenameFromUrl, PercentDecodesSegment) {
    EXPECT_EQ(filename_from_url("https://example.com/a%20b.txt"), "a b.txt");
}

TEST(FilenameFromUrl, EmptyForBareHost) {
    EXPECT_EQ(filename_from_url("https://example.com/"), "");
    EXPECT_EQ(filename_from_url("https://example.com"), "");
    EXPECT_EQ(filename_from_url("https://example.com?x=1"), "");
}

TEST(FilenameFromUrl, DecodedSeparatorsCannotEscape) {
    EXPECT_EQ(filename_from_url("https://example.com/%2E%2E%2Fetc%2Fpasswd"), "passwd");
}

} // namespace

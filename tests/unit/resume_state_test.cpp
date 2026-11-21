#include <flowget/resume_state.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

using flowget::kMaxResumeSegments;
using flowget::kMaxResumeStateBytes;
using flowget::kResumeSchemaVersion;
using flowget::parse_resume_state;
using flowget::ResumeParseResult;
using flowget::ResumeSegment;
using flowget::ResumeState;
using flowget::serialize_resume_state;

constexpr std::int64_t kMiB = 1024 * 1024;

ResumeState valid_state() {
    ResumeState state;
    state.url = "https://example.com/big.bin";
    state.total_bytes = 4 * kMiB;
    state.etag = "\"abc123\"";
    state.last_modified = "Tue, 17 Nov 2026 10:00:00 GMT";
    state.segments = {{0, 2 * kMiB, 2 * kMiB}, {2 * kMiB, 4 * kMiB, kMiB}};
    return state;
}

// The canonical serialization of valid_state(), used as the base for mutation tests. Keeping it as
// text (instead of building json objects) means the tests also pin the on-disk shape.
std::string valid_json() {
    return R"({
  "schema_version": 1,
  "url": "https://example.com/big.bin",
  "total_bytes": 4194304,
  "etag": "\"abc123\"",
  "last_modified": "Tue, 17 Nov 2026 10:00:00 GMT",
  "segments": [
    {"start": 0, "end": 2097152, "done": 2097152},
    {"start": 2097152, "end": 4194304, "done": 1048576}
  ]
})";
}

std::string replace_once(std::string text, std::string_view from, std::string_view to) {
    const std::size_t position = text.find(from);
    EXPECT_NE(position, std::string::npos) << "test fixture does not contain " << from;
    text.replace(position, from.size(), to);
    return text;
}

struct RejectedCase {
    const char* name;
    std::string json;
};

void expect_rejected(const RejectedCase& test_case) {
    const ResumeParseResult result = parse_resume_state(test_case.json);
    EXPECT_FALSE(result.ok()) << test_case.name;
    EXPECT_FALSE(result.error.empty()) << test_case.name;
}

TEST(ResumeState, RoundTripsThroughTheCanonicalForm) {
    const ResumeState original = valid_state();
    const std::string text = serialize_resume_state(original);

    const ResumeParseResult parsed = parse_resume_state(text);
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_EQ(parsed.state.schema_version, kResumeSchemaVersion);
    EXPECT_EQ(parsed.state.url, original.url);
    EXPECT_EQ(parsed.state.total_bytes, original.total_bytes);
    EXPECT_EQ(parsed.state.etag, original.etag);
    EXPECT_EQ(parsed.state.last_modified, original.last_modified);
    ASSERT_EQ(parsed.state.segments.size(), original.segments.size());
    for (std::size_t index = 0; index < original.segments.size(); ++index) {
        EXPECT_EQ(parsed.state.segments[index].start, original.segments[index].start);
        EXPECT_EQ(parsed.state.segments[index].end, original.segments[index].end);
        EXPECT_EQ(parsed.state.segments[index].done, original.segments[index].done);
    }
}

TEST(ResumeState, SerializationIsDeterministicAndEndsWithOneNewline) {
    const std::string first = serialize_resume_state(valid_state());
    const std::string second = serialize_resume_state(valid_state());
    EXPECT_EQ(first, second);
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(first.back(), '\n');
    EXPECT_NE(first.find("\"schema_version\": 1"), std::string::npos);
    // Fixed key order: schema_version before url before total_bytes before segments.
    EXPECT_LT(first.find("\"schema_version\""), first.find("\"url\""));
    EXPECT_LT(first.find("\"url\""), first.find("\"total_bytes\""));
    EXPECT_LT(first.find("\"etag\""), first.find("\"segments\""));
}

TEST(ResumeState, AcceptsTheHandWrittenFixture) {
    const ResumeParseResult parsed = parse_resume_state(valid_json());
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_EQ(parsed.state.bytes_done(), 3 * kMiB);
}

TEST(ResumeState, RejectsNonObjectAndGarbageInput) {
    for (const char* text : {"", "   ", "not json", "[]", "null", "42", "{\"a\": }"}) {
        const ResumeParseResult result = parse_resume_state(text);
        EXPECT_FALSE(result.ok()) << text;
    }
}

TEST(ResumeState, RejectsAnOversizedFileWithoutParsingIt) {
    const std::string oversized(kMaxResumeStateBytes + 1, 'x');
    const ResumeParseResult result = parse_resume_state(oversized);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("1 MiB"), std::string::npos);
}

TEST(ResumeState, RejectsMissingOrUnexpectedFields) {
    std::vector<RejectedCase> cases = {
        {"extra field",
         replace_once(valid_json(), "\"segments\": [", "\"extra\": 1,\n  \"segments\": [")},
        {"missing url",
         replace_once(valid_json(), "  \"url\": \"https://example.com/big.bin\",\n", "")},
        {"missing etag", replace_once(valid_json(), "  \"etag\": \"\\\"abc123\\\"\",\n", "")},
        {"missing segments",
         replace_once(valid_json(), ",\n  \"segments\": [", ",\n  \"other\": [")},
    };
    for (const RejectedCase& test_case : cases) {
        expect_rejected(test_case);
    }
}

TEST(ResumeState, RejectsUnsupportedSchemaVersion) {
    std::vector<RejectedCase> cases = {
        {"version 2", replace_once(valid_json(), "\"schema_version\": 1", "\"schema_version\": 2")},
        {"version 0", replace_once(valid_json(), "\"schema_version\": 1", "\"schema_version\": 0")},
        {"version as string",
         replace_once(valid_json(), "\"schema_version\": 1", "\"schema_version\": \"1\"")},
        {"version as float",
         replace_once(valid_json(), "\"schema_version\": 1", "\"schema_version\": 1.0")},
    };
    for (const RejectedCase& test_case : cases) {
        expect_rejected(test_case);
    }
}

TEST(ResumeState, RejectsInvalidUrlAndSize) {
    std::vector<RejectedCase> cases = {
        {"empty url", replace_once(valid_json(), "\"https://example.com/big.bin\"", "\"\"")},
        {"url is a number", replace_once(valid_json(), "\"https://example.com/big.bin\"", "42")},
        {"zero size", replace_once(valid_json(), "\"total_bytes\": 4194304", "\"total_bytes\": 0")},
        {"negative size",
         replace_once(valid_json(), "\"total_bytes\": 4194304", "\"total_bytes\": -1")},
        {"size as string",
         replace_once(valid_json(), "\"total_bytes\": 4194304", "\"total_bytes\": \"4194304\"")},
        {"size as float",
         replace_once(valid_json(), "\"total_bytes\": 4194304", "\"total_bytes\": 4194304.5")},
    };
    for (const RejectedCase& test_case : cases) {
        expect_rejected(test_case);
    }
}

TEST(ResumeState, RejectsMissingOrWeakValidators) {
    // A weak ETag proves semantic equivalence only, so If-Range must not use it. Without
    // Last-Modified the sidecar cannot prove the bytes still belong to the same representation.
    std::vector<RejectedCase> cases = {
        {"no validator at all",
         replace_once(replace_once(valid_json(), "\"etag\": \"\\\"abc123\\\"\"", "\"etag\": \"\""),
                      "\"last_modified\": \"Tue, 17 Nov 2026 10:00:00 GMT\"",
                      "\"last_modified\": \"\"")},
        {"weak etag only",
         replace_once(replace_once(valid_json(),
                                   "\"etag\": \"\\\"abc123\\\"\"",
                                   "\"etag\": \"W/\\\"abc123\\\"\""),
                      "\"last_modified\": \"Tue, 17 Nov 2026 10:00:00 GMT\"",
                      "\"last_modified\": \"\"")},
    };
    for (const RejectedCase& test_case : cases) {
        expect_rejected(test_case);
    }
}

TEST(ResumeState, AcceptsWeakEtagWhenLastModifiedIsPresent) {
    const std::string text = replace_once(
        valid_json(), "\"etag\": \"\\\"abc123\\\"\"", "\"etag\": \"W/\\\"abc123\\\"\"");
    const ResumeParseResult result = parse_resume_state(text);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_FALSE(flowget::is_strong_etag(result.state.etag));
    EXPECT_TRUE(flowget::has_resume_validator(result.state));
}

TEST(ResumeState, RejectsBrokenSegmentGeometry) {
    std::vector<RejectedCase> cases = {
        {"gap between segments",
         replace_once(valid_json(),
                      "{\"start\": 2097152, \"end\": 4194304, \"done\": 1048576}",
                      "{\"start\": 2097153, \"end\": 4194304, \"done\": 1048576}")},
        {"does not start at zero",
         replace_once(
             valid_json(), "{\"start\": 0, \"end\": 2097152", "{\"start\": 1, \"end\": 2097152")},
        {"last end short of the size",
         replace_once(valid_json(),
                      "{\"start\": 2097152, \"end\": 4194304, \"done\": 1048576}",
                      "{\"start\": 2097152, \"end\": 4194303, \"done\": 1048576}")},
        {"end beyond the size",
         replace_once(valid_json(),
                      "{\"start\": 2097152, \"end\": 4194304, \"done\": 1048576}",
                      "{\"start\": 2097152, \"end\": 4194305, \"done\": 1048576}")},
        {"empty range",
         replace_once(
             valid_json(), "{\"start\": 0, \"end\": 2097152", "{\"start\": 0, \"end\": 0")},
        {"done beyond the range",
         replace_once(valid_json(),
                      "{\"start\": 2097152, \"end\": 4194304, \"done\": 1048576}",
                      "{\"start\": 2097152, \"end\": 4194304, \"done\": 2097153}")},
        {"negative done", replace_once(valid_json(), "\"done\": 1048576", "\"done\": -1")},
        {"empty segments",
         replace_once(valid_json(),
                      "[\n    {\"start\": 0, \"end\": 2097152, \"done\": 2097152},\n"
                      "    {\"start\": 2097152, \"end\": 4194304, \"done\": 1048576}\n  ]",
                      "[]")},
        {"segment missing done",
         replace_once(valid_json(),
                      "{\"start\": 2097152, \"end\": 4194304, \"done\": 1048576}",
                      "{\"start\": 2097152, \"end\": 4194304}")},
        {"segment has an extra field",
         replace_once(valid_json(),
                      "{\"start\": 2097152, \"end\": 4194304, \"done\": 1048576}",
                      "{\"start\": 2097152, \"end\": 4194304, \"done\": 1048576, \"x\": 1}")},
    };
    for (const RejectedCase& test_case : cases) {
        expect_rejected(test_case);
    }
}

TEST(ResumeState, RejectsMoreSegmentsThanTheLimit) {
    std::string json = R"({"schema_version": 1, "url": "u", "total_bytes": 5000,
  "etag": "\"e\"", "last_modified": "", "segments": [)";
    for (std::size_t index = 0; index < kMaxResumeSegments + 1; ++index) {
        if (index != 0) {
            json += ',';
        }
        json += "{\"start\": 0, \"end\": 1, \"done\": 0}";
    }
    json += "]}";

    const ResumeParseResult result = parse_resume_state(json);
    EXPECT_FALSE(result.ok());
}

TEST(ResumeState, DetectsStrongVersusWeakEtags) {
    EXPECT_TRUE(flowget::is_strong_etag("\"abc\""));
    EXPECT_TRUE(flowget::is_strong_etag("abc"));
    EXPECT_FALSE(flowget::is_strong_etag(""));
    EXPECT_FALSE(flowget::is_strong_etag("W/\"abc\""));
    EXPECT_FALSE(flowget::is_strong_etag("w/\"abc\""));
    EXPECT_TRUE(flowget::is_strong_etag("W\"abc\""));
}

TEST(ResumeState, ComputesBytesDone) {
    ResumeState state = valid_state();
    EXPECT_EQ(state.bytes_done(), 3 * kMiB);
    state.segments[1].done = 0;
    EXPECT_EQ(state.bytes_done(), 2 * kMiB);
    state.segments.clear();
    EXPECT_EQ(state.bytes_done(), 0);
}

TEST(ResumeState, BuildsSidecarPathsNextToTheOutput) {
    EXPECT_EQ(flowget::resume_sidecar_path("out.bin"), "out.bin.flowget");
    EXPECT_EQ(flowget::resume_sidecar_temp_path("out.bin"), "out.bin.flowget.tmp");
    const std::string non_ascii = "İndirilenler/İndirilen_çğış.bin";
    EXPECT_EQ(flowget::resume_sidecar_path(non_ascii), non_ascii + ".flowget");
}

// Every accepted sidecar must describe a file the engine can resume safely. Rejected input only
// needs a non-empty reason; the parser is the security boundary, so it may never crash or throw.
void expect_parser_invariants(const ResumeParseResult& result) {
    if (!result.ok()) {
        EXPECT_FALSE(result.error.empty());
        return;
    }
    const ResumeState& state = result.state;
    ASSERT_FALSE(state.segments.empty());
    EXPECT_LE(state.segments.size(), kMaxResumeSegments);
    EXPECT_EQ(state.schema_version, kResumeSchemaVersion);
    EXPECT_GT(state.total_bytes, 0);
    EXPECT_FALSE(state.url.empty());
    EXPECT_TRUE(flowget::has_resume_validator(state));

    std::int64_t next_start = 0;
    for (const ResumeSegment& segment : state.segments) {
        EXPECT_EQ(segment.start, next_start);
        EXPECT_GT(segment.end, segment.start);
        EXPECT_LE(segment.end, state.total_bytes);
        EXPECT_GE(segment.done, 0);
        EXPECT_LE(segment.done, segment.length());
        next_start = segment.end;
    }
    EXPECT_EQ(next_start, state.total_bytes);
}

TEST(ResumeState, EveryTruncationEitherParsesOrFailsCleanly) {
    const std::string text = valid_json();
    for (std::size_t length = 0; length <= text.size(); ++length) {
        SCOPED_TRACE("truncated at " + std::to_string(length));
        expect_parser_invariants(parse_resume_state(std::string_view{text}.substr(0, length)));
    }
}

TEST(ResumeState, FixedSeedMutationsNeverProduceAnInvalidState) {
    const std::string original = valid_json();
    std::mt19937 engine{0xC0FFEEU};
    std::uniform_int_distribution<std::size_t> position(0, original.size() - 1);
    std::uniform_int_distribution<int> byte_value(0, 255);
    std::uniform_int_distribution<int> edits(1, 3);

    for (int iteration = 0; iteration < 1000; ++iteration) {
        std::string mutated = original;
        const int count = edits(engine);
        for (int edit = 0; edit < count; ++edit) {
            mutated[position(engine)] = static_cast<char>(byte_value(engine));
        }
        SCOPED_TRACE("mutation " + std::to_string(iteration));
        expect_parser_invariants(parse_resume_state(mutated));
    }
}

} // namespace

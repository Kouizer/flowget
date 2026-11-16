#include <flowget/segment_planner.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace {

using flowget::FileOffset;
using flowget::plan_segments;
using flowget::Segment;

constexpr FileOffset kMiB = 1024 * 1024;

// Invariants every plan must satisfy, checked in one place so each scenario only states what makes
// it special.
void expect_valid_plan(const std::vector<Segment>& segments, FileOffset total, int connections) {
    if (total <= 0) {
        EXPECT_TRUE(segments.empty());
        return;
    }
    ASSERT_FALSE(segments.empty());

    const FileOffset min_size = flowget::minimum_segment_size(total);
    const FileOffset max_by_size = std::max<FileOffset>(total / min_size, 1);
    const FileOffset expected_count = std::min<FileOffset>(
        static_cast<FileOffset>(std::clamp(connections, 1, flowget::kMaxConnections)), max_by_size);
    EXPECT_EQ(segments.size(), static_cast<std::size_t>(expected_count));

    FileOffset next_offset = 0;
    FileOffset smallest = std::numeric_limits<FileOffset>::max();
    FileOffset largest = 0;
    for (const Segment& segment : segments) {
        EXPECT_EQ(segment.offset, next_offset) << "ranges must be contiguous and gap-free";
        EXPECT_GT(segment.length, 0);
        EXPECT_LE(segment.length,
                  static_cast<FileOffset>(std::numeric_limits<std::int64_t>::max()));
        smallest = std::min(smallest, segment.length);
        largest = std::max(largest, segment.length);
        next_offset += segment.length;
    }
    EXPECT_EQ(next_offset, total) << "segments must cover the whole resource";
    EXPECT_LE(largest - smallest, 1) << "segment lengths must differ by at most one byte";

    if (segments.size() > 1) {
        EXPECT_GE(smallest, min_size) << "multi-segment plans must respect the minimum size";
    }
}

TEST(SegmentPlanner, ZeroAndNegativeSizeYieldEmptyPlan) {
    EXPECT_TRUE(plan_segments(0, 8).empty());
    EXPECT_TRUE(plan_segments(-1, 8).empty());
    EXPECT_TRUE(plan_segments(std::numeric_limits<FileOffset>::min(), 8).empty());
}

TEST(SegmentPlanner, SmallFileStaysOneSegment) {
    // Below 2 MiB the minimum segment size does not leave room for a second piece, so the plan is a
    // single range and the downloader takes the single-stream path.
    for (const FileOffset size : {FileOffset{1}, kMiB - 1, kMiB, 2 * kMiB - 1}) {
        const std::vector<Segment> segments = plan_segments(size, 8);
        ASSERT_EQ(segments.size(), 1U) << "size=" << size;
        EXPECT_EQ(segments.front().offset, 0);
        EXPECT_EQ(segments.front().length, size);
        expect_valid_plan(segments, size, 8);
    }
}

TEST(SegmentPlanner, SplitsEvenlyWhenDivisible) {
    constexpr FileOffset kSize = 100 * kMiB;
    const std::vector<Segment> segments = plan_segments(kSize, 8);
    ASSERT_EQ(segments.size(), 8U);
    for (const Segment& segment : segments) {
        EXPECT_EQ(segment.length, kSize / 8);
    }
    expect_valid_plan(segments, kSize, 8);
}

TEST(SegmentPlanner, RemainderGoesToTheFirstSegments) {
    constexpr FileOffset kSize = 3 * kMiB + 7;
    const std::vector<Segment> segments = plan_segments(kSize, 3);
    ASSERT_EQ(segments.size(), 3U);
    // 3 MiB + 7 = 3 segments of 1 MiB + 2 bytes, remainder 1.
    EXPECT_EQ(segments[0].length, kMiB + 2 + 1);
    EXPECT_EQ(segments[1].length, kMiB + 2);
    EXPECT_EQ(segments[2].length, kMiB + 2);
    expect_valid_plan(segments, kSize, 3);
}

TEST(SegmentPlanner, ConnectionCountIsClamped) {
    constexpr FileOffset kSize = 64 * kMiB;
    for (const int connections : {0, -1, -1000}) {
        const std::vector<Segment> segments = plan_segments(kSize, connections);
        ASSERT_EQ(segments.size(), 1U) << "connections=" << connections;
        expect_valid_plan(segments, kSize, connections);
    }
    for (const int connections : {flowget::kMaxConnections, flowget::kMaxConnections + 1, 10'000}) {
        const std::vector<Segment> segments = plan_segments(kSize, connections);
        EXPECT_EQ(segments.size(), static_cast<std::size_t>(flowget::kMaxConnections))
            << "connections=" << connections;
        expect_valid_plan(segments, kSize, connections);
    }
}

TEST(SegmentPlanner, MoreConnectionsThanPiecesYieldsFewerSegments) {
    // 3 MiB with a 1 MiB minimum: at most 3 pieces no matter how many connections are requested.
    const std::vector<Segment> segments = plan_segments(3 * kMiB, 16);
    EXPECT_EQ(segments.size(), 3U);
    expect_valid_plan(segments, 3 * kMiB, 16);
}

TEST(SegmentPlanner, MinimumSegmentSizeGrowsWithTheResource) {
    EXPECT_EQ(flowget::minimum_segment_size(1), kMiB);
    EXPECT_EQ(flowget::minimum_segment_size(64 * kMiB), kMiB);
    EXPECT_EQ(flowget::minimum_segment_size(128 * kMiB), 2 * kMiB);
    EXPECT_EQ(flowget::minimum_segment_size(1024 * kMiB), 16 * kMiB);
    // Negative totals only exist on the error path; the planner never uses the value there.
    EXPECT_EQ(flowget::minimum_segment_size(-5), kMiB);
}

TEST(SegmentPlanner, HandlesOneByteAndPrimeSizes) {
    for (const FileOffset size : {FileOffset{1},
                                  FileOffset{2},
                                  FileOffset{1'000'003},
                                  3 * kMiB + 1,
                                  7 * kMiB + 13,
                                  13 * kMiB + 1}) {
        for (const int connections : {1, 2, 3, 5, 8, 64}) {
            expect_valid_plan(plan_segments(size, connections), size, connections);
        }
    }
}

TEST(SegmentPlanner, MoreConnectionsNeverReduceTheSegmentCount) {
    constexpr FileOffset kSize = 500 * kMiB;
    std::size_t previous = 0;
    for (int connections = 1; connections <= flowget::kMaxConnections; ++connections) {
        const std::vector<Segment> segments = plan_segments(kSize, connections);
        EXPECT_GE(segments.size(), previous) << "connections=" << connections;
        previous = segments.size();
        expect_valid_plan(segments, kSize, connections);
    }
}

TEST(SegmentPlanner, LargeResourceStaysWithinConnectionBudget) {
    constexpr FileOffset kSize = 5LL * 1024 * 1024 * 1024; // 5 GiB
    const std::vector<Segment> segments = plan_segments(kSize, 16);
    ASSERT_EQ(segments.size(), 16U);
    expect_valid_plan(segments, kSize, 16);
    EXPECT_EQ(segments.front().length, kSize / 16);
}

TEST(SegmentPlanner, HandlesSizesNearTheInt64Limit) {
    constexpr FileOffset kSize = std::numeric_limits<FileOffset>::max();
    for (const int connections : {1, 8, 64}) {
        const std::vector<Segment> segments = plan_segments(kSize, connections);
        expect_valid_plan(segments, kSize, connections);
    }
}

} // namespace

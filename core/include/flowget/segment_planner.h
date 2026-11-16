#ifndef FLOWGET_SEGMENT_PLANNER_H
#define FLOWGET_SEGMENT_PLANNER_H

#include <flowget/storage.h>

#include <cstdint>
#include <vector>

namespace flowget {

// Concurrent connections for one download. The CLI validates the same range; the library clamps
// instead of failing so a caller cannot turn a tuning mistake into a failed download.
constexpr int kMaxConnections = 64;

// Minimum useful piece size. Smaller pieces multiply request overhead and leave the dynamic
// splitter (S7) nothing to work with.
constexpr FileOffset kMinSegmentSize = 1024 * 1024;

struct Segment {
    FileOffset offset = 0;
    FileOffset length = 0;
};

// max(1 MiB, total_size / 64): a 64 MiB download gets 1 MiB pieces, a 1 GiB download 16 MiB
// pieces. This keeps the segment count bounded by the connection budget instead of exploding on
// large files.
FileOffset minimum_segment_size(FileOffset total_size) noexcept;

// Splits [0, total_size) into contiguous, gap-free ranges of near-equal length so that N workers
// can write disjoint byte ranges of one file (S4).
//
// The count is clamp(connections, 1, total_size / minimum_segment_size(total_size)) and never
// exceeds kMaxConnections. Start offsets are ascending, the lengths differ by at most one byte, the
// first (total_size % count) segments carry the extra byte, and the lengths sum to total_size.
// `total_size <= 0` yields an empty plan: there is nothing to fetch (the downloader creates an
// empty file instead).
std::vector<Segment> plan_segments(FileOffset total_size, int connections);

} // namespace flowget

#endif /* FLOWGET_SEGMENT_PLANNER_H */

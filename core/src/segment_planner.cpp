#include <flowget/segment_planner.h>

#include <algorithm>

namespace flowget {

FileOffset minimum_segment_size(FileOffset total_size) noexcept {
    return std::max<FileOffset>(kMinSegmentSize, total_size / 64);
}

std::vector<Segment> plan_segments(FileOffset total_size, int connections) {
    std::vector<Segment> segments;
    if (total_size <= 0) {
        return segments;
    }

    const FileOffset min_size = minimum_segment_size(total_size);
    const FileOffset by_size = std::max<FileOffset>(total_size / min_size, 1);
    const FileOffset requested =
        std::clamp<FileOffset>(static_cast<FileOffset>(std::clamp(connections, 1, kMaxConnections)),
                               FileOffset{1},
                               by_size);
    const auto count = static_cast<std::size_t>(requested);

    const FileOffset base = total_size / requested;
    const FileOffset remainder = total_size % requested;

    segments.reserve(count);
    FileOffset offset = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const FileOffset extra = static_cast<FileOffset>(index) < remainder ? 1 : 0;
        const FileOffset length = base + extra;
        segments.push_back(Segment{offset, length});
        offset += length;
    }
    return segments;
}

} // namespace flowget

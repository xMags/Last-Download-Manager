#include "media/MediaModel.h"

namespace ldm::media {

double TotalSeconds(const TrackSegments& track)
{
    double seconds = 0;
    for (const Segment& segment : track.segments) {
        seconds += segment.seconds;
    }
    return seconds;
}

}  // namespace ldm::media

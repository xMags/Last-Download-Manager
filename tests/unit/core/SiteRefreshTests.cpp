#include "core/SiteRefresh.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using ldm::core::AddressExpiresAtUnixMs;
using ldm::core::ApplyFreshAddresses;
using ldm::core::NeedsFreshAddresses;
using ldm::model::Download;
using ldm::model::MediaTrack;

constexpr std::int64_t kExpire = 1'792'909'519;  // seconds

std::string Address(const char* itag, std::int64_t expire = kExpire)
{
    return std::string("https://rr1.media.test/videoplayback?itag=") + itag + "&expire=" + std::to_string(expire) + "&sig=x";
}

MediaTrack Track(const char* format, std::int64_t bytes, std::int64_t done)
{
    MediaTrack track;
    track.sourceUrl = "https://www.youtube.com/watch?v=aqz-KE-bpKQ";
    track.wholeFile = true;
    ldm::model::MediaPiece piece;
    piece.url = Address(format);
    track.pieces.push_back(piece);
    track.siteFormat = format;
    track.expectedBytes = bytes;
    if (done > 0) {
        track.ranges = {ldm::model::Segment{0, bytes, done}};
    }
    return track;
}

Download SiteVideo()
{
    Download d;
    d.kind = ldm::model::DownloadKind::Dash;
    d.siteVideo = true;
    d.url = "https://www.youtube.com/watch?v=aqz-KE-bpKQ";
    d.tracks = {Track("299", 257'619'653, 50'000'000), Track("140", 10'271'496, 10'271'496)};
    return d;
}

// A lookup answer offering `format` with `bytes` at a new address.
ldm::media::SiteVideo Answer(std::vector<std::pair<std::string, std::optional<std::int64_t>>> formats)
{
    ldm::media::SiteVideo video;
    video.id = "aqz-KE-bpKQ";
    for (const auto& [id, bytes] : formats) {
        ldm::media::TrackSegments whole;
        ldm::media::Segment segment;
        segment.url = Address(id.c_str(), kExpire + 21'600);
        whole.segments.push_back(segment);
        if (id == "140") {
            ldm::media::Rendition audio;
            audio.source = whole;
            audio.site = ldm::media::SiteFormat{id, bytes};
            video.offer.audio.push_back(audio);
        } else {
            ldm::media::Variant variant;
            variant.source = whole;
            variant.site = ldm::media::SiteFormat{id, bytes};
            video.offer.variants.push_back(variant);
        }
    }
    return video;
}

TEST(SiteRefresh, ExpiryIsReadFromTheAddress)
{
    EXPECT_EQ(AddressExpiresAtUnixMs(Address("140")), kExpire * 1000);
    EXPECT_FALSE(AddressExpiresAtUnixMs("https://media.test/v?itag=140"));
    EXPECT_FALSE(AddressExpiresAtUnixMs("https://media.test/v?expire=soon"));
    EXPECT_FALSE(AddressExpiresAtUnixMs("https://media.test/v?expire=12"));
    EXPECT_FALSE(AddressExpiresAtUnixMs("https://media.test/v?noexpire=1792909519"));
    EXPECT_FALSE(AddressExpiresAtUnixMs("not an address"));
}

TEST(SiteRefresh, FreshAddressesAreNeededTenMinutesBeforeTheyExpire)
{
    const Download d = SiteVideo();
    EXPECT_FALSE(NeedsFreshAddresses(d, kExpire * 1000 - 11 * 60'000));
    EXPECT_TRUE(NeedsFreshAddresses(d, kExpire * 1000 - 9 * 60'000));
    EXPECT_TRUE(NeedsFreshAddresses(d, kExpire * 1000 + 3'600'000));
    Download plain = d;
    plain.siteVideo = false;
    EXPECT_FALSE(NeedsFreshAddresses(plain, kExpire * 1000 + 3'600'000)) << "only site videos";
}

TEST(SiteRefresh, TheSameFormatsContinueFromTheBytesOnDisk)
{
    Download d = SiteVideo();
    const auto fresh = ApplyFreshAddresses(d, Answer({{"299", 257'619'653}, {"140", 10'271'496}, {"137", 1}}));
    ASSERT_FALSE(fresh.failure);
    EXPECT_EQ(fresh.changed, (std::vector<std::uint32_t>{0, 1}));
    EXPECT_TRUE(fresh.restarted.empty());
    EXPECT_EQ(d.tracks[0].pieces.front().url, Address("299", kExpire + 21'600));
    EXPECT_EQ(d.tracks[0].sourceUrl, "https://www.youtube.com/watch?v=aqz-KE-bpKQ") << "still the page";
    ASSERT_EQ(d.tracks[0].ranges.size(), 1u);
    EXPECT_EQ(d.tracks[0].ranges[0].done, 50'000'000);
    EXPECT_FALSE(NeedsFreshAddresses(d, kExpire * 1000 + 3'600'000));
}

TEST(SiteRefresh, AFormatOfAnotherLengthStartsAgain)
{
    Download d = SiteVideo();
    const auto fresh = ApplyFreshAddresses(d, Answer({{"299", 257'619'000}, {"140", 10'271'496}}));
    ASSERT_FALSE(fresh.failure);
    EXPECT_EQ(fresh.restarted, (std::vector<std::uint32_t>{0}));
    EXPECT_TRUE(d.tracks[0].ranges.empty()) << "different bytes are never put together";
    EXPECT_EQ(d.tracks[0].expectedBytes, 257'619'000);
    EXPECT_EQ(d.tracks[1].ranges.size(), 1u);

    // A length the site no longer states proves nothing either.
    Download unknown = SiteVideo();
    const auto unsure = ApplyFreshAddresses(unknown, Answer({{"299", std::nullopt}, {"140", 10'271'496}}));
    EXPECT_EQ(unsure.restarted, (std::vector<std::uint32_t>{0}));
}

TEST(SiteRefresh, AFormatNoLongerOfferedChangesNothing)
{
    Download d = SiteVideo();
    const Download before = d;
    const auto fresh = ApplyFreshAddresses(d, Answer({{"140", 10'271'496}}));
    ASSERT_TRUE(fresh.failure);
    EXPECT_TRUE(fresh.changed.empty());
    EXPECT_EQ(d.tracks[1].pieces.front().url, before.tracks[1].pieces.front().url) << "not even the format that is there";
}

TEST(SiteRefresh, OnlyRefusedAddressesLookExpired)
{
    using ldm::core::FailureClass;
    using ldm::core::FailureReason;
    using ldm::core::TransferFailure;
    EXPECT_TRUE(ldm::core::LooksLikeExpiredAddress(TransferFailure{FailureReason::PermissionDenied, FailureClass::Fatal, 403, ""}));
    EXPECT_TRUE(ldm::core::LooksLikeExpiredAddress(TransferFailure{FailureReason::NotFound, FailureClass::Fatal, 410, ""}));
    EXPECT_FALSE(ldm::core::LooksLikeExpiredAddress(TransferFailure{FailureReason::NotFound, FailureClass::Fatal, 404, ""}));
    EXPECT_FALSE(ldm::core::LooksLikeExpiredAddress(TransferFailure{FailureReason::Dns, FailureClass::Retryable, 0, ""}));
}

}  // namespace

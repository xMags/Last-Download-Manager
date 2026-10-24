#include "media/DashManifest.h"

#include "media/DrmSystems.h"
#include "media/NumberText.h"
#include "media/UrlReference.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ldm::media {

namespace {

constexpr std::string_view kTrickModeScheme = "http://dashif.org/guidelines/trickmode";
constexpr std::string_view kRoleScheme = "urn:mpeg:dash:role:2011";
constexpr std::string_view kChannelScheme = "urn:mpeg:dash:23003:3:audio_channel_configuration:2011";
// Widest zero padding a template may ask for ("%05d" is typical).
constexpr std::uint64_t kMaxTemplateWidth = 32;
constexpr std::uint64_t kMaxPictureSide = 100'000;
constexpr std::uint64_t kMaxChannels = 64;
// Segment counts are worked out in floating point first; anything above this is refused before
// it is turned into an integer.
constexpr double kMaxComputedCount = 1e15;
// Segment ends that miss a period's end by less than this (in segments) are rounding, not a segment.
constexpr double kCountTolerance = 1e-6;

base::Error Malformed(std::string message)
{
    return base::MakeError(base::ErrorCode::Protocol, std::move(message));
}

base::Error Refused(std::string message)
{
    return base::MakeError(base::ErrorCode::Unsupported, std::move(message));
}

std::string_view TrimSpace(std::string_view text)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool IsDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

// xs:duration as DASH uses it ("PT1H2M3.5S", "P1DT2H"). Years and months have no fixed length,
// so only zero values of them are accepted.
std::optional<double> ParseXsDuration(std::string_view text)
{
    text = TrimSpace(text);
    if (!text.starts_with('P')) {
        return std::nullopt;
    }
    text.remove_prefix(1);
    bool inTime = false;
    bool any = false;
    double seconds = 0;
    while (!text.empty()) {
        if (text.front() == 'T') {
            if (inTime || text.size() == 1) {
                return std::nullopt;
            }
            inTime = true;
            text.remove_prefix(1);
            continue;
        }
        size_t length = 0;
        while (length < text.size() && (IsDigit(text[length]) || text[length] == '.')) {
            ++length;
        }
        if (length == 0 || length == text.size()) {
            return std::nullopt;
        }
        const auto value = ParseNonNegativeDecimal(text.substr(0, length));
        const char unit = text[length];
        text.remove_prefix(length + 1);
        if (!value) {
            return std::nullopt;
        }
        if (!inTime && unit == 'D') {
            seconds += *value * 86400;
        } else if (!inTime && (unit == 'Y' || unit == 'M')) {
            if (*value != 0) {
                return std::nullopt;
            }
        } else if (inTime && unit == 'H') {
            seconds += *value * 3600;
        } else if (inTime && unit == 'M') {
            seconds += *value * 60;
        } else if (inTime && unit == 'S') {
            seconds += *value;
        } else {
            return std::nullopt;
        }
        any = true;
    }
    if (!any || !std::isfinite(seconds)) {
        return std::nullopt;
    }
    return seconds;
}

// xs:dateTime ("2026-10-24T10:00:00Z", "2026-10-24T12:00:00.5+02:00") as seconds since 1970 UTC.
// A time without a zone is taken as UTC, as DASH players do.
std::optional<double> ParseXsDateTime(std::string_view text)
{
    text = TrimSpace(text);
    if (text.size() < 19 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':') {
        return std::nullopt;
    }
    const auto field = [text](size_t position, size_t length) -> std::optional<int> {
        const auto value = ParseUnsigned(text.substr(position, length));
        return value ? std::optional<int>(static_cast<int>(*value)) : std::nullopt;
    };
    const auto year = field(0, 4);
    const auto month = field(5, 2);
    const auto day = field(8, 2);
    const auto hour = field(11, 2);
    const auto minute = field(14, 2);
    const auto second = field(17, 2);
    if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59 || *second > 60) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{*year}, std::chrono::month{static_cast<unsigned>(*month)},
                                           std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    std::string_view rest = text.substr(19);
    double fraction = 0;
    if (rest.starts_with('.')) {
        size_t length = 1;
        while (length < rest.size() && IsDigit(rest[length])) {
            ++length;
        }
        const auto value = ParseNonNegativeDecimal(rest.substr(0, length));
        if (!value || length == 1) {
            return std::nullopt;
        }
        fraction = *value;
        rest.remove_prefix(length);
    }
    double offset = 0;
    if (rest == "Z" || rest.empty()) {
        offset = 0;
    } else if (rest.size() == 6 && (rest[0] == '+' || rest[0] == '-') && rest[3] == ':') {
        const auto zoneHours = ParseUnsigned(rest.substr(1, 2));
        const auto zoneMinutes = ParseUnsigned(rest.substr(4, 2));
        if (!zoneHours || !zoneMinutes || *zoneHours > 14 || *zoneMinutes > 59) {
            return std::nullopt;
        }
        offset = static_cast<double>(*zoneHours * 3600 + *zoneMinutes * 60) * (rest[0] == '-' ? -1 : 1);
    } else {
        return std::nullopt;
    }
    const auto days = std::chrono::sys_days(date).time_since_epoch().count();
    return static_cast<double>(days) * 86400 + *hour * 3600 + *minute * 60 + *second + fraction - offset;
}

double EpochSeconds(std::chrono::system_clock::time_point time)
{
    return std::chrono::duration<double>(time.time_since_epoch()).count();
}

// "25" or "30000/1001".
std::optional<double> ParseFrameRate(std::string_view text)
{
    const size_t slash = text.find('/');
    const auto numerator = ParseUnsigned(text.substr(0, slash));
    const auto denominator = slash == std::string_view::npos ? std::optional<std::uint64_t>(1)
                                                             : ParseUnsigned(text.substr(slash + 1));
    if (!numerator || !denominator || *numerator == 0 || *denominator == 0) {
        return std::nullopt;
    }
    return static_cast<double>(*numerator) / static_cast<double>(*denominator);
}

// "<first>-<last>", both inclusive.
std::optional<ByteRange> ParseRange(std::string_view text)
{
    const size_t dash = text.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto first = ParseUnsigned(TrimSpace(text.substr(0, dash)));
    const auto last = ParseUnsigned(TrimSpace(text.substr(dash + 1)));
    if (!first || !last || *last < *first || *last - *first == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return ByteRange{*first, *last - *first + 1};
}

// An optional unsigned attribute: nothing when absent, an error when present but not a number.
base::Result<std::optional<std::uint64_t>> UnsignedAttribute(const XmlElement& element, std::string_view name)
{
    const std::string* text = element.Attribute(name);
    if (!text) {
        return std::optional<std::uint64_t>();
    }
    const auto value = ParseUnsigned(TrimSpace(*text));
    if (!value) {
        return Malformed(element.name + "@" + std::string(name) + " is not a number");
    }
    return std::optional<std::uint64_t>(*value);
}

base::Result<std::optional<double>> DurationAttribute(const XmlElement& element, std::string_view name)
{
    const std::string* text = element.Attribute(name);
    if (!text) {
        return std::optional<double>();
    }
    const auto value = ParseXsDuration(*text);
    if (!value) {
        return Malformed(element.name + "@" + std::string(name) + " is not a duration LDM can read");
    }
    return std::optional<double>(*value);
}

// The values a SegmentTemplate address may contain.
struct TemplateValues {
    const std::string& representationId;
    std::uint64_t bandwidth = 0;
    std::optional<std::uint64_t> number;
    std::optional<std::uint64_t> time;
};

// Fills in $RepresentationID$, $Number$, $Bandwidth$ and $Time$ (each but the first optionally
// with %0<width>d) and $$. Nothing for an unknown identifier, a malformed one, or one whose value
// does not apply (such as $Number$ in an initialization address).
std::optional<std::string> ExpandTemplate(std::string_view pattern, const TemplateValues& values)
{
    std::string expanded;
    while (true) {
        const size_t open = pattern.find('$');
        if (open == std::string_view::npos) {
            expanded.append(pattern);
            return expanded;
        }
        expanded.append(pattern.substr(0, open));
        const size_t close = pattern.find('$', open + 1);
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view identifier = pattern.substr(open + 1, close - open - 1);
        pattern.remove_prefix(close + 1);
        if (identifier.empty()) {
            expanded.push_back('$');
            continue;
        }
        const size_t percent = identifier.find('%');
        const std::string_view name = identifier.substr(0, percent);
        if (name == "RepresentationID") {
            if (percent != std::string_view::npos) {
                return std::nullopt;
            }
            expanded.append(values.representationId);
            continue;
        }
        std::uint64_t width = 0;
        if (percent != std::string_view::npos) {
            const std::string_view format = identifier.substr(percent);
            if (format.size() < 4 || format[1] != '0' || format.back() != 'd') {
                return std::nullopt;
            }
            const auto parsed = ParseUnsigned(format.substr(2, format.size() - 3));
            if (!parsed || *parsed > kMaxTemplateWidth) {
                return std::nullopt;
            }
            width = *parsed;
        }
        std::optional<std::uint64_t> value;
        if (name == "Number") {
            value = values.number;
        } else if (name == "Bandwidth") {
            value = values.bandwidth;
        } else if (name == "Time") {
            value = values.time;
        }
        if (!value) {
            return std::nullopt;
        }
        const std::string digits = std::to_string(*value);
        if (digits.size() < width) {
            expanded.append(static_cast<size_t>(width) - digits.size(), '0');
        }
        expanded.append(digits);
    }
}

enum class Addressing {
    Template,
    List,
    Base,  // SegmentBase, or no segment information: one file
};

// The segment information that applies to a representation: the elements of the most specific
// kind found, most specific level first, so attributes are looked up as DASH inherits them.
struct SegmentInfo {
    Addressing kind = Addressing::Base;
    std::vector<const XmlElement*> elements;

    [[nodiscard]] const std::string* Attribute(std::string_view name) const
    {
        for (const XmlElement* element : elements) {
            if (const std::string* value = element->Attribute(name)) {
                return value;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const XmlElement* Child(std::string_view name) const
    {
        for (const XmlElement* element : elements) {
            if (const XmlElement* child = element->Child(name)) {
                return child;
            }
        }
        return nullptr;
    }

    [[nodiscard]] base::Result<std::optional<std::uint64_t>> Unsigned(std::string_view name) const
    {
        for (const XmlElement* element : elements) {
            if (element->Attribute(name)) {
                return UnsignedAttribute(*element, name);
            }
        }
        return std::optional<std::uint64_t>();
    }
};

SegmentInfo FindSegmentInfo(std::initializer_list<const XmlElement*> levels)
{
    constexpr std::array<std::pair<Addressing, std::string_view>, 3> kKinds{{
        {Addressing::Template, "SegmentTemplate"},
        {Addressing::List, "SegmentList"},
        {Addressing::Base, "SegmentBase"},
    }};
    SegmentInfo info;
    std::string_view elementName;
    for (const XmlElement* level : levels) {
        for (const auto& [kind, name] : kKinds) {
            if (level->Child(name)) {
                info.kind = kind;
                elementName = name;
                break;
            }
        }
        if (!elementName.empty()) {
            break;
        }
    }
    if (elementName.empty()) {
        return info;
    }
    for (const XmlElement* level : levels) {
        if (const XmlElement* element = level->Child(elementName)) {
            info.elements.push_back(element);
        }
    }
    return info;
}

// What a track's segments need to know about time.
struct Timing {
    bool dynamic = false;
    double availabilityStart = 0;  // seconds since 1970; dynamic MPDs only
    std::optional<double> timeShiftBuffer;
    double now = 0;  // seconds since 1970
    double periodStart = 0;
    std::optional<double> periodSeconds;

    // How far into the period the live edge is; dynamic MPDs only.
    [[nodiscard]] double LiveSeconds() const
    {
        const double elapsed = now - availabilityStart - periodStart;
        return periodSeconds ? std::min(elapsed, *periodSeconds) : elapsed;
    }
};

struct TimelineEntry {
    std::uint64_t time = 0;
    std::uint64_t duration = 0;
};

// A SegmentTimeline expanded into its segments, and how many were skipped at the start (outside a
// live window), so segment numbers can still be worked out.
struct ExpandedTimeline {
    std::vector<TimelineEntry> entries;
    std::uint64_t skipped = 0;
};

base::Result<ExpandedTimeline> ExpandTimeline(const XmlElement& timeline, std::uint64_t timescale,
                                              std::uint64_t timeOffset, const Timing& timing)
{
    struct Run {
        std::uint64_t time = 0;
        std::uint64_t duration = 0;
        std::uint64_t count = 0;
    };
    const auto points = timeline.Children("S");
    const double scale = static_cast<double>(timescale);
    const double offset = static_cast<double>(timeOffset);
    std::vector<Run> runs;
    std::uint64_t current = 0;
    std::uint64_t total = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        const XmlElement& point = *points[i];
        auto pointTime = UnsignedAttribute(point, "t");
        auto pointDuration = UnsignedAttribute(point, "d");
        if (!pointTime || !pointDuration) {
            return !pointTime ? pointTime.GetError() : pointDuration.GetError();
        }
        if (pointTime.Value()) {
            if (*pointTime.Value() < current) {
                return Malformed("the SegmentTimeline goes back in time");
            }
            current = *pointTime.Value();
        }
        if (!pointDuration.Value() || *pointDuration.Value() == 0) {
            return Malformed("an S element of the SegmentTimeline needs a positive d");
        }
        const std::uint64_t d = *pointDuration.Value();
        std::uint64_t count = 1;
        const std::string* repeat = point.Attribute("r");
        if (repeat && TrimSpace(*repeat) == "-1") {
            // Repeats until the next S element's time, the period's end or (live) the live edge.
            double end = 0;
            bool completeOnly = false;
            const auto nextTime = i + 1 < points.size() ? UnsignedAttribute(*points[i + 1], "t")
                                                        : base::Result<std::optional<std::uint64_t>>(std::nullopt);
            if (!nextTime) {
                return nextTime.GetError();
            }
            if (nextTime.Value()) {
                end = static_cast<double>(*nextTime.Value());
            } else if (timing.dynamic) {
                end = offset + timing.LiveSeconds() * scale;
                completeOnly = true;
            } else if (timing.periodSeconds) {
                end = offset + *timing.periodSeconds * scale;
            } else {
                return Malformed("r=\"-1\" in a period of unknown length");
            }
            const double repeats = (end - static_cast<double>(current)) / static_cast<double>(d);
            const double computed = repeats <= 0 ? 0 : (completeOnly ? std::floor(repeats + kCountTolerance)
                                                                      : std::ceil(repeats - kCountTolerance));
            if (computed > kMaxComputedCount) {
                return Malformed("the SegmentTimeline repeats beyond any real length");
            }
            count = static_cast<std::uint64_t>(computed);
        } else if (repeat) {
            const auto repeats = ParseUnsigned(TrimSpace(*repeat));
            if (!repeats || *repeats >= static_cast<std::uint64_t>(kMaxComputedCount)) {
                return Malformed("S@r is not a repeat count LDM accepts");
            }
            count = *repeats + 1;
        }
        const auto newTotal = CheckedAdd(total, count);
        if (!newTotal || (!timing.dynamic && *newTotal > kMaxSegmentsPerTrack)) {
            return Malformed("the SegmentTimeline has more than " + std::to_string(kMaxSegmentsPerTrack) + " segments");
        }
        if (count != 0 && d > (std::numeric_limits<std::uint64_t>::max() - current) / count) {
            return Malformed("the SegmentTimeline's times overflow");
        }
        total = *newTotal;
        runs.push_back(Run{current, d, count});
        current += d * count;
    }

    // A live timeline keeps only what is inside its time-shift window, and at most the cap.
    std::uint64_t skipped = 0;
    if (timing.dynamic) {
        if (timing.timeShiftBuffer) {
            const double windowStart = offset + (timing.LiveSeconds() - *timing.timeShiftBuffer) * scale;
            for (const Run& run : runs) {
                // Segments of this run that ended before the window opened.
                const double ended = std::floor((windowStart - static_cast<double>(run.time)) /
                                                static_cast<double>(run.duration));
                if (ended > 0) {
                    skipped += std::min(run.count, static_cast<std::uint64_t>(std::min(ended, kMaxComputedCount)));
                }
            }
        }
        if (total - skipped > kMaxSegmentsPerTrack) {
            skipped = total - kMaxSegmentsPerTrack;
        }
    }
    ExpandedTimeline expanded;
    expanded.skipped = skipped;
    expanded.entries.reserve(static_cast<size_t>(total - skipped));
    std::uint64_t index = 0;  // of the run's first segment, counted from the timeline's start
    for (const Run& run : runs) {
        const std::uint64_t begin = skipped > index ? std::min(run.count, skipped - index) : 0;
        for (std::uint64_t k = begin; k < run.count; ++k) {
            expanded.entries.push_back(TimelineEntry{run.time + k * run.duration, run.duration});
        }
        index += run.count;
    }
    return expanded;
}

// The segment numbers and media times of a SegmentTemplate with @duration and no timeline.
struct NumberedRange {
    std::uint64_t first = 0;  // index from startNumber
    std::uint64_t count = 0;  // segments from `first`
};

base::Result<NumberedRange> DurationSegments(double segmentSeconds, const Timing& timing)
{
    if (!timing.dynamic) {
        if (!timing.periodSeconds) {
            return Malformed("the length of the period is unknown, so its segments cannot be counted");
        }
        const double computed = std::ceil(*timing.periodSeconds / segmentSeconds - kCountTolerance);
        if (computed > static_cast<double>(kMaxSegmentsPerTrack)) {
            return Malformed("the period has more than " + std::to_string(kMaxSegmentsPerTrack) + " segments");
        }
        return NumberedRange{0, static_cast<std::uint64_t>(std::max(computed, 0.0))};
    }
    // Live: the segments that are complete at `now`, inside the time-shift window.
    const double live = timing.LiveSeconds();
    const double completed = live <= 0 ? 0 : std::floor(live / segmentSeconds + kCountTolerance);
    double first = 0;
    if (timing.timeShiftBuffer) {
        first = std::max(0.0, std::floor((live - *timing.timeShiftBuffer) / segmentSeconds));
    }
    if (completed > kMaxComputedCount) {
        return Malformed("the live stream claims more segments than any real stream has");
    }
    first = std::min(first, completed);
    if (completed - first > static_cast<double>(kMaxSegmentsPerTrack)) {
        first = completed - static_cast<double>(kMaxSegmentsPerTrack);
    }
    return NumberedRange{static_cast<std::uint64_t>(first), static_cast<std::uint64_t>(completed - first)};
}

base::Result<std::string> ResolveOrFail(const std::string& base, std::string_view reference, std::string_view what)
{
    auto url = ResolveMediaUrl(base, TrimSpace(reference));
    if (!url) {
        return Malformed(std::string(what) + " is not an http or https address");
    }
    return std::move(*url);
}

// The address that `element`'s BaseURL (its first; others are alternatives) makes of `base`.
base::Result<std::string> ApplyBaseUrl(const std::string& base, const XmlElement& element)
{
    const XmlElement* baseUrl = element.Child("BaseURL");
    if (!baseUrl) {
        return base;
    }
    return ResolveOrFail(base, baseUrl->text, "a BaseURL");
}

// One representation's download list.
base::Result<TrackSegments> BuildTrack(const SegmentInfo& info, const std::string& baseUrl,
                                       const std::string& representationId, std::uint64_t bandwidth,
                                       const Timing& timing)
{
    TrackSegments track;
    track.live = timing.dynamic;
    const auto finish = [&track]() {
        for (const Segment& segment : track.segments) {
            track.targetSeconds = std::max(track.targetSeconds, segment.seconds);
        }
    };

    if (info.kind == Addressing::Base) {
        // The whole file; its init section and index are inside it.
        Segment whole;
        whole.url = baseUrl;
        whole.seconds = timing.periodSeconds.value_or(0);
        whole.sequence = 1;
        track.segments.push_back(std::move(whole));
        finish();
        return track;
    }

    auto timescaleValue = info.Unsigned("timescale");
    auto offsetValue = info.Unsigned("presentationTimeOffset");
    auto startValue = info.Unsigned("startNumber");
    auto durationValue = info.Unsigned("duration");
    auto endValue = info.Unsigned("endNumber");
    for (const auto* value : {&timescaleValue, &offsetValue, &startValue, &durationValue, &endValue}) {
        if (!*value) {
            return value->GetError();
        }
    }
    const std::uint64_t timescale = timescaleValue.Value().value_or(1);
    const std::uint64_t timeOffset = offsetValue.Value().value_or(0);
    const std::uint64_t startNumber = startValue.Value().value_or(1);
    if (timescale == 0) {
        return Malformed("@timescale is zero");
    }
    const double scale = static_cast<double>(timescale);

    // The init section: SegmentTemplate@initialization, or an Initialization element.
    const TemplateValues initValues{representationId, bandwidth, std::nullopt, std::nullopt};
    const std::string* initPattern = info.kind == Addressing::Template ? info.Attribute("initialization") : nullptr;
    if (initPattern) {
        const auto expanded = ExpandTemplate(*initPattern, initValues);
        if (!expanded) {
            return Malformed("SegmentTemplate@initialization is not a template LDM can fill in");
        }
        auto url = ResolveOrFail(baseUrl, *expanded, "the init section's address");
        if (!url) {
            return url.GetError();
        }
        track.inits.push_back(InitSection{std::move(url).Value(), std::nullopt});
    } else if (const XmlElement* initialization = info.Child("Initialization")) {
        InitSection init;
        const std::string* source = initialization->Attribute("sourceURL");
        auto url = source ? ResolveOrFail(baseUrl, *source, "Initialization@sourceURL") : base::Result<std::string>(baseUrl);
        if (!url) {
            return url.GetError();
        }
        init.url = std::move(url).Value();
        if (const std::string* range = initialization->Attribute("range")) {
            init.range = ParseRange(*range);
            if (!init.range) {
                return Malformed("Initialization@range is not <first>-<last>");
            }
        }
        track.inits.push_back(std::move(init));
    }
    const std::optional<size_t> initIndex = track.inits.empty() ? std::nullopt : std::optional<size_t>(0);

    const XmlElement* timelineElement = info.Child("SegmentTimeline");
    std::optional<ExpandedTimeline> timeline;
    if (timelineElement) {
        auto expanded = ExpandTimeline(*timelineElement, timescale, timeOffset, timing);
        if (!expanded) {
            return expanded.GetError();
        }
        timeline = std::move(expanded).Value();
    }

    const auto numberAt = [startNumber](std::uint64_t index) { return CheckedAdd(startNumber, index); };

    if (info.kind == Addressing::List) {
        std::vector<const XmlElement*> urls;
        for (const XmlElement* element : info.elements) {
            urls = element->Children("SegmentURL");
            if (!urls.empty()) {
                break;
            }
        }
        if (urls.empty()) {
            return Malformed("a SegmentList without SegmentURL elements");
        }
        if (urls.size() > kMaxSegmentsPerTrack) {
            return Malformed("the SegmentList has more than " + std::to_string(kMaxSegmentsPerTrack) + " segments");
        }
        const std::optional<std::uint64_t> duration = durationValue.Value();
        for (size_t i = 0; i < urls.size(); ++i) {
            Segment segment;
            const std::string* media = urls[i]->Attribute("media");
            auto url = media ? ResolveOrFail(baseUrl, *media, "SegmentURL@media") : base::Result<std::string>(baseUrl);
            if (!url) {
                return url.GetError();
            }
            segment.url = std::move(url).Value();
            if (const std::string* range = urls[i]->Attribute("mediaRange")) {
                segment.range = ParseRange(*range);
                if (!segment.range) {
                    return Malformed("SegmentURL@mediaRange is not <first>-<last>");
                }
            }
            if (timeline && i < timeline->entries.size()) {
                segment.seconds = static_cast<double>(timeline->entries[i].duration) / scale;
            } else if (duration && *duration != 0) {
                segment.seconds = static_cast<double>(*duration) / scale;
            } else if (timing.periodSeconds) {
                segment.seconds = *timing.periodSeconds / static_cast<double>(urls.size());
            }
            const auto number = numberAt(i);
            if (!number) {
                return Malformed("the segment numbers overflow");
            }
            segment.sequence = *number;
            segment.init = initIndex;
            track.segments.push_back(std::move(segment));
        }
        finish();
        return track;
    }

    // SegmentTemplate.
    const std::string* media = info.Attribute("media");
    if (!media) {
        return Malformed("a SegmentTemplate without @media");
    }
    const auto addSegment = [&track, &numberAt, media, &representationId, bandwidth, &baseUrl, initIndex](
                                std::uint64_t index, std::uint64_t time, double seconds) -> base::Status {
        const auto number = numberAt(index);
        if (!number) {
            return Malformed("the segment numbers overflow");
        }
        const auto expanded = ExpandTemplate(*media, TemplateValues{representationId, bandwidth, number, time});
        if (!expanded) {
            return Malformed("SegmentTemplate@media is not a template LDM can fill in");
        }
        auto url = ResolveOrFail(baseUrl, *expanded, "a segment's address");
        if (!url) {
            return url.GetError();
        }
        Segment segment;
        segment.url = std::move(url).Value();
        segment.seconds = seconds;
        segment.sequence = *number;
        segment.init = initIndex;
        track.segments.push_back(std::move(segment));
        return {};
    };

    if (timeline) {
        track.segments.reserve(timeline->entries.size());
        for (size_t i = 0; i < timeline->entries.size(); ++i) {
            const TimelineEntry& entry = timeline->entries[i];
            const base::Status added =
                addSegment(timeline->skipped + i, entry.time, static_cast<double>(entry.duration) / scale);
            if (!added) {
                return added.GetError();
            }
        }
        finish();
        return track;
    }

    const std::optional<std::uint64_t> duration = durationValue.Value();
    if (!duration || *duration == 0) {
        return Malformed("a SegmentTemplate with neither a positive @duration nor a SegmentTimeline");
    }
    const double segmentSeconds = static_cast<double>(*duration) / scale;
    auto range = DurationSegments(segmentSeconds, timing);
    if (!range) {
        return range.GetError();
    }
    std::uint64_t count = range.Value().count;
    if (const auto endNumber = endValue.Value()) {
        // @endNumber is the number of the last segment there is.
        const std::uint64_t first = range.Value().first;
        if (*endNumber < startNumber || *endNumber - startNumber < first) {
            count = 0;
        } else {
            count = std::min(count, *endNumber - startNumber - first + 1);
        }
    }
    track.segments.reserve(static_cast<size_t>(count));
    for (std::uint64_t k = 0; k < count; ++k) {
        const std::uint64_t index = range.Value().first + k;
        double seconds = segmentSeconds;
        if (!timing.dynamic && timing.periodSeconds) {
            // The last segment ends with the period.
            const double remaining = *timing.periodSeconds - static_cast<double>(index) * segmentSeconds;
            seconds = std::clamp(remaining, 0.0, segmentSeconds);
        }
        if (index != 0 && *duration > (std::numeric_limits<std::uint64_t>::max() - timeOffset) / index) {
            return Malformed("the segment times overflow");
        }
        const base::Status added = addSegment(index, timeOffset + index * *duration, seconds);
        if (!added) {
            return added.GetError();
        }
    }
    finish();
    return track;
}

enum class TrackKind {
    Video,
    Audio,
    Text,
};

const std::string* Inherited(const XmlElement& representation, const XmlElement& set, std::string_view name)
{
    const std::string* value = representation.Attribute(name);
    return value ? value : set.Attribute(name);
}

std::optional<TrackKind> ClassifySet(const XmlElement& set)
{
    if (const std::string* contentType = set.Attribute("contentType")) {
        if (*contentType == "video") {
            return TrackKind::Video;
        }
        if (*contentType == "audio") {
            return TrackKind::Audio;
        }
        if (*contentType == "text") {
            return TrackKind::Text;
        }
        return std::nullopt;  // image (thumbnails) and anything newer
    }
    const XmlElement* first = set.Child("Representation");
    const std::string* mimeType = set.Attribute("mimeType");
    if (!mimeType && first) {
        mimeType = first->Attribute("mimeType");
    }
    const std::string* codecs = set.Attribute("codecs");
    if (!codecs && first) {
        codecs = first->Attribute("codecs");
    }
    if (!mimeType) {
        return std::nullopt;
    }
    if (mimeType->starts_with("video/")) {
        return TrackKind::Video;
    }
    if (mimeType->starts_with("audio/")) {
        return TrackKind::Audio;
    }
    if (mimeType->starts_with("text/") || *mimeType == "application/ttml+xml") {
        return TrackKind::Text;
    }
    if (*mimeType == "application/mp4" && codecs && (codecs->starts_with("stpp") || codecs->starts_with("wvtt"))) {
        return TrackKind::Text;
    }
    return std::nullopt;
}

bool IsTrickMode(const XmlElement& element)
{
    for (const XmlElement* property : element.Children("EssentialProperty")) {
        const std::string* scheme = property->Attribute("schemeIdUri");
        if (scheme && *scheme == kTrickModeScheme) {
            return true;
        }
    }
    return false;
}

bool HasRole(const XmlElement& set, std::string_view value)
{
    for (const XmlElement* role : set.Children("Role")) {
        const std::string* scheme = role->Attribute("schemeIdUri");
        const std::string* roleValue = role->Attribute("value");
        if (scheme && *scheme == kRoleScheme && roleValue && *roleValue == value) {
            return true;
        }
    }
    return false;
}

base::Result<std::optional<int>> Channels(const XmlElement& representation, const XmlElement& set)
{
    for (const XmlElement* level : {&representation, &set}) {
        for (const XmlElement* configuration : level->Children("AudioChannelConfiguration")) {
            const std::string* scheme = configuration->Attribute("schemeIdUri");
            const std::string* value = configuration->Attribute("value");
            if (!scheme || *scheme != kChannelScheme || !value) {
                continue;  // other schemes code layouts, not counts
            }
            const auto count = ParseUnsigned(TrimSpace(*value));
            if (!count || *count == 0 || *count > kMaxChannels) {
                return Malformed("AudioChannelConfiguration@value is not a channel count");
            }
            return std::optional<int>(static_cast<int>(*count));
        }
    }
    return std::optional<int>();
}

void CollectProtection(const XmlElement& element, std::optional<DrmInfo>& drm)
{
    for (const XmlElement* protection : element.Children("ContentProtection")) {
        const std::string* scheme = protection->Attribute("schemeIdUri");
        AddDrmSystem(drm, scheme ? DrmSystemName(*scheme) : std::string("unknown DRM"));
    }
}

// One representation as an offer entry, with what identifies it across periods.
struct Entry {
    std::string representationId;
    TrackKind kind = TrackKind::Video;
    Variant variant;      // kind == Video
    Rendition rendition;  // otherwise

    TrackSegments& Segments()
    {
        return std::get<TrackSegments>(kind == TrackKind::Video ? variant.source : *rendition.source);
    }
};

base::Result<Entry> BuildEntry(const XmlElement& representation, const XmlElement& set, TrackKind kind,
                               std::string groupId, TrackSegments segments)
{
    Entry entry;
    entry.kind = kind;
    entry.representationId = *representation.Attribute("id");
    auto bandwidth = UnsignedAttribute(representation, "bandwidth");
    if (!bandwidth) {
        return bandwidth.GetError();
    }
    const std::string* codecs = Inherited(representation, set, "codecs");
    const std::string* mimeType = Inherited(representation, set, "mimeType");
    if (kind == TrackKind::Video) {
        Variant& variant = entry.variant;
        variant.bandwidth = bandwidth.Value().value_or(0);
        variant.codecs = codecs ? *codecs : std::string();
        variant.mimeType = mimeType ? *mimeType : std::string();
        const std::string* width = Inherited(representation, set, "width");
        const std::string* height = Inherited(representation, set, "height");
        if (width && height) {
            const auto w = ParseUnsigned(TrimSpace(*width));
            const auto h = ParseUnsigned(TrimSpace(*height));
            if (!w || !h || *w == 0 || *h == 0 || *w > kMaxPictureSide || *h > kMaxPictureSide) {
                return Malformed("the representation's width or height is not a picture size");
            }
            variant.resolution = Resolution{static_cast<int>(*w), static_cast<int>(*h)};
        }
        if (const std::string* frameRate = Inherited(representation, set, "frameRate")) {
            variant.frameRate = ParseFrameRate(TrimSpace(*frameRate));
            if (!variant.frameRate) {
                return Malformed("@frameRate is not a frame rate");
            }
        }
        variant.source = std::move(segments);
        return entry;
    }
    Rendition& rendition = entry.rendition;
    rendition.groupId = std::move(groupId);
    const std::string* language = Inherited(representation, set, "lang");
    rendition.language = language ? *language : std::string();
    const XmlElement* label = representation.Child("Label");
    if (!label) {
        label = set.Child("Label");
    }
    rendition.name = label ? std::string(TrimSpace(label->text)) : rendition.language;
    if (rendition.name.empty()) {
        rendition.name = rendition.groupId;
    }
    rendition.isDefault = HasRole(set, "main");
    rendition.forced = HasRole(set, "forced-subtitle");
    rendition.bandwidth = bandwidth.Value().value_or(0);
    rendition.codecs = codecs ? *codecs : std::string();
    rendition.mimeType = mimeType ? *mimeType : std::string();
    if (kind == TrackKind::Audio) {
        auto channels = Channels(representation, set);
        if (!channels) {
            return channels.GetError();
        }
        rendition.channels = channels.Value();
    }
    rendition.source = SegmentSource{std::move(segments)};
    return entry;
}

base::Result<std::vector<Entry>> ReadPeriod(const XmlElement& period, const std::string& mpdBase, const Timing& timing,
                                            std::optional<DrmInfo>& drm)
{
    auto periodBase = ApplyBaseUrl(mpdBase, period);
    if (!periodBase) {
        return periodBase.GetError();
    }
    std::vector<Entry> entries;
    const auto sets = period.Children("AdaptationSet");
    for (size_t setIndex = 0; setIndex < sets.size(); ++setIndex) {
        const XmlElement& set = *sets[setIndex];
        const auto kind = ClassifySet(set);
        if (!kind || IsTrickMode(set)) {
            continue;
        }
        auto setBase = ApplyBaseUrl(periodBase.Value(), set);
        if (!setBase) {
            return setBase.GetError();
        }
        CollectProtection(set, drm);
        const std::string* setId = set.Attribute("id");
        const std::string groupId = setId ? *setId : "set" + std::to_string(setIndex + 1);
        for (const XmlElement* representation : set.Children("Representation")) {
            if (IsTrickMode(*representation)) {
                continue;
            }
            CollectProtection(*representation, drm);
            const std::string* id = representation->Attribute("id");
            if (!id || id->empty() || !representation->Attribute("bandwidth")) {
                return Malformed("a Representation needs @id and @bandwidth");
            }
            const bool duplicate = std::any_of(entries.begin(), entries.end(),
                                               [id](const Entry& entry) { return entry.representationId == *id; });
            if (duplicate) {
                return Malformed("two Representations in one period have the id \"" + *id + "\"");
            }
            auto bandwidth = UnsignedAttribute(*representation, "bandwidth");
            if (!bandwidth) {
                return bandwidth.GetError();
            }
            auto base = ApplyBaseUrl(setBase.Value(), *representation);
            if (!base) {
                return base.GetError();
            }
            const SegmentInfo info = FindSegmentInfo({representation, &set, &period});
            auto track = BuildTrack(info, base.Value(), *id, bandwidth.Value().value_or(0), timing);
            if (!track) {
                return track.GetError();
            }
            auto entry = BuildEntry(*representation, set, *kind, groupId, std::move(track).Value());
            if (!entry) {
                return entry.GetError();
            }
            entries.push_back(std::move(entry).Value());
        }
    }
    return entries;
}

// Appends a later period's segments to the first period's entries: the same representations,
// continued, each period starting a new timeline.
base::Status JoinPeriod(std::vector<Entry>& joined, std::vector<Entry> later)
{
    const auto ids = [](const std::vector<Entry>& entries) {
        std::vector<std::string> list;
        for (const Entry& entry : entries) {
            list.push_back(entry.representationId);
        }
        std::sort(list.begin(), list.end());
        return list;
    };
    if (ids(joined) != ids(later)) {
        return Refused("the periods of this DASH stream carry different content (often inserted ads); LDM "
                       "cannot join them into one file");
    }
    for (Entry& entry : joined) {
        const auto match = std::find_if(later.begin(), later.end(), [&entry](const Entry& candidate) {
            return candidate.representationId == entry.representationId;
        });
        TrackSegments& target = entry.Segments();
        TrackSegments& source = match->Segments();
        if (target.segments.size() + source.segments.size() > kMaxSegmentsPerTrack) {
            return Malformed("the joined periods have more than " + std::to_string(kMaxSegmentsPerTrack) + " segments");
        }
        const size_t initOffset = target.inits.size();
        for (InitSection& init : source.inits) {
            target.inits.push_back(std::move(init));
        }
        for (size_t i = 0; i < source.segments.size(); ++i) {
            Segment segment = std::move(source.segments[i]);
            if (segment.init) {
                *segment.init += initOffset;
            }
            segment.discontinuity = segment.discontinuity || i == 0;
            target.segments.push_back(std::move(segment));
        }
        target.targetSeconds = std::max(target.targetSeconds, source.targetSeconds);
    }
    return {};
}

}  // namespace

base::Result<MediaOffer> ParseMpd(const XmlElement& mpd, std::string_view mpdUrl,
                                  std::chrono::system_clock::time_point now)
{
    if (mpd.name != "MPD") {
        return Malformed("not a DASH manifest (the root element is not MPD)");
    }
    const auto manifestUrl = ResolveMediaUrl(mpdUrl, "");
    if (!manifestUrl) {
        return base::MakeError(base::ErrorCode::InvalidArgument, "the manifest's address is not an http or https address");
    }
    MediaOffer offer;
    offer.format = StreamFormat::Dash;

    Timing timing;
    timing.now = EpochSeconds(now);
    const std::string* type = mpd.Attribute("type");
    if (type && *type != "static" && *type != "dynamic") {
        return Malformed("MPD@type is neither static nor dynamic");
    }
    timing.dynamic = type && *type == "dynamic";
    offer.live = timing.dynamic;
    if (timing.dynamic) {
        const std::string* start = mpd.Attribute("availabilityStartTime");
        const auto parsed = start ? ParseXsDateTime(*start) : std::nullopt;
        if (!parsed) {
            return Malformed("a dynamic MPD needs a readable availabilityStartTime");
        }
        timing.availabilityStart = *parsed;
        auto window = DurationAttribute(mpd, "timeShiftBufferDepth");
        if (!window) {
            return window.GetError();
        }
        timing.timeShiftBuffer = window.Value();
    }
    auto totalSeconds = DurationAttribute(mpd, "mediaPresentationDuration");
    if (!totalSeconds) {
        return totalSeconds.GetError();
    }

    auto mpdBase = ApplyBaseUrl(*manifestUrl, mpd);
    if (!mpdBase) {
        return mpdBase.GetError();
    }

    const auto periods = mpd.Children("Period");
    if (periods.empty()) {
        return Malformed("the MPD has no Period");
    }
    // When each period starts and how long it lasts (ISO/IEC 23009-1 section 5.3.2.1).
    std::vector<double> starts(periods.size());
    std::vector<std::optional<double>> lengths(periods.size());
    for (size_t i = 0; i < periods.size(); ++i) {
        auto start = DurationAttribute(*periods[i], "start");
        auto length = DurationAttribute(*periods[i], "duration");
        if (!start || !length) {
            return !start ? start.GetError() : length.GetError();
        }
        lengths[i] = length.Value();
        if (start.Value()) {
            starts[i] = *start.Value();
        } else if (i == 0) {
            starts[i] = 0;
        } else if (lengths[i - 1]) {
            starts[i] = starts[i - 1] + *lengths[i - 1];
        } else {
            return Malformed("the start of period " + std::to_string(i + 1) + " cannot be worked out");
        }
    }
    for (size_t i = 0; i < periods.size(); ++i) {
        if (lengths[i]) {
            continue;
        }
        if (i + 1 < periods.size() && periods[i + 1]->Attribute("start")) {
            lengths[i] = std::max(0.0, starts[i + 1] - starts[i]);
        } else if (i + 1 == periods.size() && totalSeconds.Value()) {
            lengths[i] = std::max(0.0, *totalSeconds.Value() - starts[i]);
        }
    }

    std::vector<Entry> entries;
    for (size_t i = 0; i < periods.size(); ++i) {
        timing.periodStart = starts[i];
        timing.periodSeconds = lengths[i];
        auto periodEntries = ReadPeriod(*periods[i], mpdBase.Value(), timing, offer.drm);
        if (!periodEntries) {
            return periodEntries.GetError();
        }
        if (i == 0) {
            entries = std::move(periodEntries).Value();
            continue;
        }
        const base::Status joined = JoinPeriod(entries, std::move(periodEntries).Value());
        if (!joined) {
            return joined.GetError();
        }
    }

    if (totalSeconds.Value()) {
        offer.seconds = totalSeconds.Value();
    } else if (std::all_of(lengths.begin(), lengths.end(), [](const auto& length) { return length.has_value(); })) {
        double sum = 0;
        for (const auto& length : lengths) {
            sum += *length;
        }
        offer.seconds = sum;
    }

    for (Entry& entry : entries) {
        if (offer.drm) {
            // Nothing of a protected stream can be downloaded; see TrackSegments::drm.
            TrackSegments& segments = entry.Segments();
            TrackSegments empty;
            empty.live = segments.live;
            empty.drm = offer.drm;
            segments = std::move(empty);
        }
        if (entry.kind == TrackKind::Video) {
            offer.variants.push_back(std::move(entry.variant));
        } else if (entry.kind == TrackKind::Audio) {
            offer.audio.push_back(std::move(entry.rendition));
        } else {
            offer.subtitles.push_back(std::move(entry.rendition));
        }
    }
    return offer;
}

}  // namespace ldm::media

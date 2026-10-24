#include "grabber/HtmlLinks.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace ldm::grabber {

namespace {

char ToLowerAscii(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IsSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return ToLowerAscii(x) == ToLowerAscii(y); });
}

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept
{
    return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

// Finds `needle` (lower case) in `text` from `from`, ignoring case. npos when absent.
std::size_t FindIgnoreCase(std::string_view text, std::string_view needle, std::size_t from) noexcept
{
    if (needle.empty() || text.size() < needle.size()) {
        return std::string_view::npos;
    }
    for (std::size_t i = from; i + needle.size() <= text.size(); ++i) {
        if (StartsWithIgnoreCase(text.substr(i), needle)) {
            return i;
        }
    }
    return std::string_view::npos;
}

void AppendUtf8(std::string& out, std::uint32_t codePoint)
{
    if (codePoint == 0 || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
        codePoint = 0xFFFD;
    }
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

constexpr std::array<std::pair<std::string_view, std::uint32_t>, 7> kNamedEntities{{
    {"amp", '&'},
    {"lt", '<'},
    {"gt", '>'},
    {"quot", '"'},
    {"apos", '\''},
    {"nbsp", 0xA0},
    {"sol", '/'},
}};

// Text collapsed to single spaces and cut to kMaxLinkText bytes on a character boundary.
class TextCollector {
public:
    void Add(std::string_view decoded)
    {
        for (const char c : decoded) {
            if (full_) {
                return;
            }
            if (IsSpace(c)) {
                pendingSpace_ = !text_.empty();
                continue;
            }
            if (pendingSpace_) {
                Push(' ');
                pendingSpace_ = false;
            }
            Push(c);
        }
    }
    [[nodiscard]] std::string Take()
    {
        std::string text = std::move(text_);
        text_.clear();
        pendingSpace_ = false;
        full_ = false;
        return text;
    }
    [[nodiscard]] bool Empty() const noexcept { return text_.empty(); }

private:
    void Push(char c)
    {
        if (text_.size() >= kMaxLinkText) {
            // Drop a character cut in two at the end.
            while (!text_.empty() && (static_cast<unsigned char>(text_.back()) & 0xC0) == 0x80) {
                text_.pop_back();
            }
            if (!text_.empty() && static_cast<unsigned char>(text_.back()) >= 0xC0) {
                text_.pop_back();
            }
            full_ = true;
            return;
        }
        text_.push_back(c);
    }

    std::string text_;
    bool pendingSpace_ = false;
    bool full_ = false;
};

struct Attribute {
    std::string_view name;
    std::string_view value;  // raw, as written
    std::size_t valueBegin = 0;  // in the page
};

struct Tag {
    std::string_view name;
    bool closing = false;
    std::vector<Attribute> attributes;
    std::size_t end = 0;  // just after '>' (or the page's end)

    [[nodiscard]] const Attribute* Find(std::string_view attribute) const
    {
        for (const Attribute& candidate : attributes) {
            if (EqualsIgnoreCase(candidate.name, attribute)) {
                return &candidate;
            }
        }
        return nullptr;
    }
};

bool IsNameChar(char c) noexcept
{
    return !IsSpace(c) && c != '>' && c != '/' && c != '=' && c != '"' && c != '\'' && c != '<';
}

// Reads the tag that starts at html[start] == '<' with a letter or '/' after it.
Tag ReadTag(std::string_view html, std::size_t start)
{
    Tag tag;
    std::size_t i = start + 1;
    if (i < html.size() && html[i] == '/') {
        tag.closing = true;
        ++i;
    }
    const std::size_t nameBegin = i;
    while (i < html.size() && IsNameChar(html[i])) {
        ++i;
    }
    tag.name = html.substr(nameBegin, i - nameBegin);
    while (i < html.size()) {
        while (i < html.size() && (IsSpace(html[i]) || html[i] == '/')) {
            ++i;
        }
        if (i >= html.size() || html[i] == '>') {
            break;
        }
        const std::size_t attributeBegin = i;
        while (i < html.size() && IsNameChar(html[i])) {
            ++i;
        }
        if (i == attributeBegin) {
            ++i;  // a stray quote or '<': skip it rather than loop
            continue;
        }
        Attribute attribute;
        attribute.name = html.substr(attributeBegin, i - attributeBegin);
        std::size_t afterName = i;
        while (afterName < html.size() && IsSpace(html[afterName])) {
            ++afterName;
        }
        if (afterName < html.size() && html[afterName] == '=') {
            i = afterName + 1;
            while (i < html.size() && IsSpace(html[i])) {
                ++i;
            }
            if (i < html.size() && (html[i] == '"' || html[i] == '\'')) {
                const char quote = html[i];
                const std::size_t valueBegin = i + 1;
                const std::size_t close = html.find(quote, valueBegin);
                const std::size_t valueEnd = close == std::string_view::npos ? html.size() : close;
                attribute.value = html.substr(valueBegin, valueEnd - valueBegin);
                attribute.valueBegin = valueBegin;
                i = close == std::string_view::npos ? html.size() : close + 1;
            } else {
                const std::size_t valueBegin = i;
                while (i < html.size() && !IsSpace(html[i]) && html[i] != '>') {
                    ++i;
                }
                attribute.value = html.substr(valueBegin, i - valueBegin);
                attribute.valueBegin = valueBegin;
            }
        } else {
            attribute.valueBegin = i;
        }
        tag.attributes.push_back(attribute);
    }
    tag.end = i < html.size() ? i + 1 : html.size();
    return tag;
}

bool IsDroppedAddress(std::string_view address)
{
    return address.empty() || address.front() == '#' || StartsWithIgnoreCase(address, "javascript:") ||
           StartsWithIgnoreCase(address, "mailto:") || StartsWithIgnoreCase(address, "data:") ||
           StartsWithIgnoreCase(address, "tel:") || StartsWithIgnoreCase(address, "about:") ||
           StartsWithIgnoreCase(address, "blob:");
}

// The trimmed part [begin, end) of `raw`, which starts at `rawBegin` in the page.
struct Span {
    std::size_t begin = 0;
    std::size_t end = 0;
};

Span Trim(std::string_view raw, std::size_t begin, std::size_t end)
{
    while (begin < end && IsSpace(raw[begin])) {
        ++begin;
    }
    while (end > begin && IsSpace(raw[end - 1])) {
        --end;
    }
    return {begin, end};
}

class Scanner {
public:
    Scanner(std::string_view html, std::size_t maxLinks) : html_(html), maxLinks_(maxLinks) {}

    PageLinks Run()
    {
        std::size_t i = 0;
        while (i < html_.size() && !result_.truncated) {
            const std::size_t open = html_.find('<', i);
            const std::size_t textEnd = open == std::string_view::npos ? html_.size() : open;
            if (openAnchor_ && textEnd > i) {
                anchorText_.Add(DecodeEntities(html_.substr(i, textEnd - i)));
            }
            if (open == std::string_view::npos) {
                break;
            }
            i = Markup(open);
        }
        CloseAnchor();
        return std::move(result_);
    }

private:
    // Handles the markup at html_[open] == '<' and returns where reading goes on.
    std::size_t Markup(std::size_t open)
    {
        const std::string_view rest = html_.substr(open);
        if (rest.starts_with("<!--")) {
            const std::size_t close = html_.find("-->", open + 4);
            return close == std::string_view::npos ? html_.size() : close + 3;
        }
        if (rest.size() < 2) {
            return html_.size();
        }
        const char next = rest[1];
        if (next == '!' || next == '?') {
            const std::size_t close = html_.find('>', open);
            return close == std::string_view::npos ? html_.size() : close + 1;
        }
        const bool letter = (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z');
        if (!letter && next != '/') {
            if (openAnchor_) {
                anchorText_.Add("<");
            }
            return open + 1;
        }
        const Tag tag = ReadTag(html_, open);
        if (tag.closing) {
            if (EqualsIgnoreCase(tag.name, "a")) {
                CloseAnchor();
            }
            return tag.end;
        }
        Element(tag);
        // Raw text elements: what is inside is not markup.
        for (const std::string_view raw : {std::string_view("script"), std::string_view("style"),
                                           std::string_view("textarea"), std::string_view("title")}) {
            if (EqualsIgnoreCase(tag.name, raw)) {
                const std::string closeTag = "</" + std::string(raw);
                const std::size_t close = FindIgnoreCase(html_, closeTag, tag.end);
                return close == std::string_view::npos ? html_.size() : close;
            }
        }
        return tag.end;
    }

    void Element(const Tag& tag)
    {
        const std::string_view name = tag.name;
        if (EqualsIgnoreCase(name, "base")) {
            if (const Attribute* href = tag.Find("href"); href != nullptr && !result_.baseHref) {
                const Span span = Trim(href->value, 0, href->value.size());
                result_.baseHref = DecodeEntities(href->value.substr(span.begin, span.end - span.begin));
            }
            return;
        }
        if (EqualsIgnoreCase(name, "a") || EqualsIgnoreCase(name, "area")) {
            if (EqualsIgnoreCase(name, "a")) {
                CloseAnchor();
            }
            const std::size_t index = result_.links.size();
            if (Add(tag.Find("href"), LinkKind::Anchor) && EqualsIgnoreCase(name, "a")) {
                openAnchor_ = index;
            } else if (EqualsIgnoreCase(name, "area")) {
                if (const Attribute* alt = tag.Find("alt"); alt != nullptr && index < result_.links.size()) {
                    TextCollector text;
                    text.Add(DecodeEntities(alt->value));
                    result_.links[index].text = text.Take();
                }
            }
            return;
        }
        if (EqualsIgnoreCase(name, "img")) {
            const std::size_t first = result_.links.size();
            Add(tag.Find("src"), LinkKind::Embedded);
            AddSrcset(tag.Find("srcset"));
            std::string alt;
            if (const Attribute* altAttribute = tag.Find("alt"); altAttribute != nullptr) {
                TextCollector text;
                text.Add(DecodeEntities(altAttribute->value));
                alt = text.Take();
            }
            for (std::size_t i = first; i < result_.links.size(); ++i) {
                result_.links[i].text = alt;
            }
            if (openAnchor_ && anchorAlt_.empty()) {
                anchorAlt_ = alt;
            }
            return;
        }
        if (EqualsIgnoreCase(name, "source")) {
            Add(tag.Find("src"), LinkKind::Embedded);
            AddSrcset(tag.Find("srcset"));
            return;
        }
        if (EqualsIgnoreCase(name, "video")) {
            Add(tag.Find("src"), LinkKind::Embedded);
            Add(tag.Find("poster"), LinkKind::Embedded);
            return;
        }
        if (EqualsIgnoreCase(name, "audio") || EqualsIgnoreCase(name, "track") || EqualsIgnoreCase(name, "embed") ||
            EqualsIgnoreCase(name, "script")) {
            Add(tag.Find("src"), LinkKind::Embedded);
            return;
        }
        if (EqualsIgnoreCase(name, "input")) {
            const Attribute* type = tag.Find("type");
            if (type != nullptr && EqualsIgnoreCase(Trimmed(type->value), "image")) {
                Add(tag.Find("src"), LinkKind::Embedded);
            }
            return;
        }
        if (EqualsIgnoreCase(name, "object")) {
            Add(tag.Find("data"), LinkKind::Embedded);
            return;
        }
        if (EqualsIgnoreCase(name, "body") || EqualsIgnoreCase(name, "table") || EqualsIgnoreCase(name, "td") ||
            EqualsIgnoreCase(name, "th")) {
            Add(tag.Find("background"), LinkKind::Embedded);
            return;
        }
        if (EqualsIgnoreCase(name, "link")) {
            const Attribute* rel = tag.Find("rel");
            const std::string relText = rel != nullptr ? DecodeEntities(rel->value) : std::string();
            const bool embedded = FindIgnoreCase(relText, "stylesheet", 0) != std::string_view::npos ||
                                  FindIgnoreCase(relText, "icon", 0) != std::string_view::npos;
            Add(tag.Find("href"), embedded ? LinkKind::Embedded : LinkKind::Anchor);
            return;
        }
        if (EqualsIgnoreCase(name, "frame") || EqualsIgnoreCase(name, "iframe")) {
            Add(tag.Find("src"), LinkKind::Frame);
            return;
        }
        if (EqualsIgnoreCase(name, "form")) {
            Add(tag.Find("action"), LinkKind::Form);
            return;
        }
        if (EqualsIgnoreCase(name, "meta")) {
            const Attribute* equiv = tag.Find("http-equiv");
            if (equiv != nullptr && EqualsIgnoreCase(Trimmed(equiv->value), "refresh")) {
                AddRefresh(tag.Find("content"));
            }
            return;
        }
    }

    static std::string_view Trimmed(std::string_view value)
    {
        const Span span = Trim(value, 0, value.size());
        return value.substr(span.begin, span.end - span.begin);
    }

    // Records the address in html_[begin, end). False when it was dropped or the cap is reached.
    bool AddRange(std::size_t begin, std::size_t end, LinkKind kind)
    {
        const Span span = Trim(html_, begin, end);
        std::string address = DecodeEntities(html_.substr(span.begin, span.end - span.begin));
        const Span decoded = Trim(address, 0, address.size());
        address = address.substr(decoded.begin, decoded.end - decoded.begin);
        if (IsDroppedAddress(address)) {
            return false;
        }
        if (result_.links.size() >= maxLinks_) {
            result_.truncated = true;
            return false;
        }
        result_.links.push_back(HtmlLink{std::move(address), kind, {}, span.begin, span.end});
        return true;
    }

    bool Add(const Attribute* attribute, LinkKind kind)
    {
        if (attribute == nullptr) {
            return false;
        }
        return AddRange(attribute->valueBegin, attribute->valueBegin + attribute->value.size(), kind);
    }

    // "a.jpg 1x, b.jpg 2x" (the HTML standard's srcset parsing, without descriptor checks).
    void AddSrcset(const Attribute* attribute)
    {
        if (attribute == nullptr) {
            return;
        }
        const std::string_view value = attribute->value;
        std::size_t i = 0;
        while (i < value.size()) {
            while (i < value.size() && (IsSpace(value[i]) || value[i] == ',')) {
                ++i;
            }
            const std::size_t begin = i;
            while (i < value.size() && !IsSpace(value[i])) {
                ++i;
            }
            std::size_t end = i;
            while (end > begin && value[end - 1] == ',') {
                --end;
            }
            const bool endedWithComma = end < i;
            if (end > begin) {
                AddRange(attribute->valueBegin + begin, attribute->valueBegin + end, LinkKind::Embedded);
            }
            if (!endedWithComma) {
                // Skip the descriptors, up to the comma outside parentheses.
                int depth = 0;
                while (i < value.size() && !(value[i] == ',' && depth == 0)) {
                    if (value[i] == '(') {
                        ++depth;
                    } else if (value[i] == ')' && depth > 0) {
                        --depth;
                    }
                    ++i;
                }
            }
        }
    }

    // content="5; url=next.html", "0;URL='next.html'" or "0; next.html".
    void AddRefresh(const Attribute* content)
    {
        if (content == nullptr) {
            return;
        }
        const std::string_view value = content->value;
        std::size_t i = 0;
        while (i < value.size() && (IsSpace(value[i]) || (value[i] >= '0' && value[i] <= '9') || value[i] == '.')) {
            ++i;
        }
        if (i >= value.size() || (value[i] != ';' && value[i] != ',')) {
            return;
        }
        ++i;
        while (i < value.size() && IsSpace(value[i])) {
            ++i;
        }
        if (StartsWithIgnoreCase(value.substr(i), "url")) {
            std::size_t j = i + 3;
            while (j < value.size() && IsSpace(value[j])) {
                ++j;
            }
            if (j < value.size() && value[j] == '=') {
                i = j + 1;
                while (i < value.size() && IsSpace(value[i])) {
                    ++i;
                }
            }
        }
        std::size_t end = value.size();
        if (i < value.size() && (value[i] == '\'' || value[i] == '"')) {
            const char quote = value[i];
            ++i;
            const std::size_t close = value.find(quote, i);
            end = close == std::string_view::npos ? value.size() : close;
        }
        AddRange(content->valueBegin + i, content->valueBegin + end, LinkKind::Refresh);
    }

    void CloseAnchor()
    {
        if (!openAnchor_) {
            return;
        }
        std::string text = anchorText_.Take();
        if (text.empty()) {
            text = std::move(anchorAlt_);
        }
        result_.links[*openAnchor_].text = std::move(text);
        openAnchor_.reset();
        anchorAlt_.clear();
    }

    std::string_view html_;
    std::size_t maxLinks_ = 0;
    PageLinks result_;
    std::optional<std::size_t> openAnchor_;  // the <a> whose text is being read
    TextCollector anchorText_;
    std::string anchorAlt_;  // the first image's alt text inside it
};

}  // namespace

std::string DecodeEntities(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t amp = text.find('&', i);
        if (amp == std::string_view::npos) {
            out.append(text.substr(i));
            break;
        }
        out.append(text.substr(i, amp - i));
        i = amp + 1;
        if (i < text.size() && text[i] == '#') {
            const bool hex = i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X');
            std::size_t j = i + (hex ? 2 : 1);
            std::uint32_t value = 0;
            const std::size_t digitsBegin = j;
            while (j < text.size() && j - digitsBegin < 8) {
                const char c = text[j];
                int digit = -1;
                if (c >= '0' && c <= '9') {
                    digit = c - '0';
                } else if (hex && c >= 'a' && c <= 'f') {
                    digit = c - 'a' + 10;
                } else if (hex && c >= 'A' && c <= 'F') {
                    digit = c - 'A' + 10;
                }
                if (digit < 0) {
                    break;
                }
                value = value * (hex ? 16 : 10) + static_cast<std::uint32_t>(digit);
                ++j;
            }
            if (j == digitsBegin) {
                out.push_back('&');
                continue;
            }
            AppendUtf8(out, value);
            i = j < text.size() && text[j] == ';' ? j + 1 : j;
            continue;
        }
        bool matched = false;
        for (const auto& [name, codePoint] : kNamedEntities) {
            if (text.substr(i).starts_with(name)) {
                const std::size_t after = i + name.size();
                // Without ';', "&ltx=1" in an address is a query parameter, not a reference (the
                // HTML standard's rule for attributes).
                const bool terminated = after < text.size() && text[after] == ';';
                if (!terminated && after < text.size()) {
                    const char c = text[after];
                    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '=') {
                        break;
                    }
                }
                AppendUtf8(out, codePoint == 0xA0 ? ' ' : codePoint);
                i = after < text.size() && text[after] == ';' ? after + 1 : after;
                matched = true;
                break;
            }
        }
        if (!matched) {
            out.push_back('&');
        }
    }
    return out;
}

PageLinks ScanLinks(std::string_view html, std::size_t maxLinks)
{
    return Scanner(html, maxLinks).Run();
}

std::string RewriteLinks(std::string_view html, std::vector<LinkReplacement> replacements)
{
    std::stable_sort(replacements.begin(), replacements.end(),
                     [](const LinkReplacement& a, const LinkReplacement& b) { return a.begin < b.begin; });
    std::string out;
    out.reserve(html.size());
    std::size_t done = 0;
    for (const LinkReplacement& replacement : replacements) {
        if (replacement.begin < done || replacement.end < replacement.begin || replacement.end > html.size()) {
            continue;
        }
        out.append(html.substr(done, replacement.begin - done));
        out.append(replacement.address);
        done = replacement.end;
    }
    out.append(html.substr(done));
    return out;
}

}  // namespace ldm::grabber

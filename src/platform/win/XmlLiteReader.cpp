#include "platform/win/XmlLiteReader.h"

#include "base/Utf.h"
#include "media/MediaModel.h"

#include <windows.h>
#include <shlwapi.h>
#include <xmllite.h>

#include <wrl/client.h>

#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ldm::platform {

namespace {

base::Error XmlError(std::string message)
{
    return base::MakeError(base::ErrorCode::Protocol, std::move(message));
}

base::Error XmlError(std::string_view what, HRESULT result)
{
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
    return XmlError(std::string(what) + " (" + code + ")");
}

std::string Utf8(const WCHAR* text, UINT length)
{
    return base::ToUtf8(std::wstring_view(text, length));
}

// Builds the tree as the reader walks the document: open elements on a stack, each moved into
// its parent when it ends.
class TreeBuilder {
public:
    void Open(media::XmlElement element) { open_.push_back(std::move(element)); }

    void Close()
    {
        media::XmlElement element = std::move(open_.back());
        open_.pop_back();
        Attach(std::move(element));
    }

    void Attach(media::XmlElement element)
    {
        if (open_.empty()) {
            root_ = std::move(element);
        } else {
            open_.back().children.push_back(std::move(element));
        }
    }

    void AppendText(std::string text)
    {
        if (!open_.empty()) {
            open_.back().text += text;
        }
    }

    [[nodiscard]] std::optional<media::XmlElement> TakeRoot() { return std::move(root_); }

private:
    std::vector<media::XmlElement> open_;
    std::optional<media::XmlElement> root_;
};

base::Result<media::XmlElement> ReadElement(IXmlReader& reader)
{
    media::XmlElement element;
    const WCHAR* text = nullptr;
    UINT length = 0;
    HRESULT result = reader.GetLocalName(&text, &length);
    if (FAILED(result)) {
        return XmlError("the XML reader gave no element name", result);
    }
    element.name = Utf8(text, length);
    for (result = reader.MoveToFirstAttribute(); result == S_OK; result = reader.MoveToNextAttribute()) {
        // Prefixed attributes and namespace declarations both have a namespace; DASH's own do not.
        if (FAILED(reader.GetNamespaceUri(&text, &length))) {
            return XmlError("the XML reader gave no attribute namespace");
        }
        if (length != 0) {
            continue;
        }
        if (FAILED(reader.GetLocalName(&text, &length))) {
            return XmlError("the XML reader gave no attribute name");
        }
        std::string name = Utf8(text, length);
        if (FAILED(reader.GetValue(&text, &length))) {
            return XmlError("the XML reader gave no attribute value");
        }
        element.attributes.emplace_back(std::move(name), Utf8(text, length));
    }
    if (FAILED(result)) {
        return XmlError("the XML attributes are malformed", result);
    }
    reader.MoveToElement();
    return element;
}

}  // namespace

base::Result<media::XmlElement> ReadXml(std::string_view bytes)
{
    if (bytes.size() > media::kMaxManifestBytes) {
        return XmlError("the document is larger than " + std::to_string(media::kMaxManifestBytes / (1024 * 1024)) +
                        " MiB");
    }
    // SHCreateMemStream copies the bytes; it takes them as BYTE, the same bytes the string holds.
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size())));
    if (!stream) {
        return base::MakeError(base::ErrorCode::Unknown, "could not create a memory stream for the XML");
    }
    Microsoft::WRL::ComPtr<IXmlReader> reader;
    HRESULT result = CreateXmlReader(IID_PPV_ARGS(reader.GetAddressOf()), nullptr);
    if (FAILED(result)) {
        return base::MakeError(base::ErrorCode::Unknown, "could not create the XML reader");
    }
    // DtdProcessing_Prohibit is XmlLite's default; set anyway so the refusal never depends on it.
    if (FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, static_cast<LONG_PTR>(kMaxXmlDepth))) ||
        FAILED(reader->SetInput(stream.Get()))) {
        return base::MakeError(base::ErrorCode::Unknown, "could not set up the XML reader");
    }

    TreeBuilder tree;
    size_t elements = 0;
    XmlNodeType type = XmlNodeType_None;
    while ((result = reader->Read(&type)) == S_OK) {
        if (type == XmlNodeType_Element) {
            if (++elements > kMaxXmlElements) {
                return XmlError("the document has more than " + std::to_string(kMaxXmlElements) + " elements");
            }
            const bool empty = reader->IsEmptyElement() != FALSE;
            auto element = ReadElement(*reader.Get());
            if (!element) {
                return element.GetError();
            }
            if (empty) {
                tree.Attach(std::move(element).Value());
            } else {
                tree.Open(std::move(element).Value());
            }
        } else if (type == XmlNodeType_EndElement) {
            tree.Close();
        } else if (type == XmlNodeType_Text || type == XmlNodeType_CDATA) {
            const WCHAR* text = nullptr;
            UINT length = 0;
            if (FAILED(reader->GetValue(&text, &length))) {
                return XmlError("the XML reader gave no text");
            }
            tree.AppendText(Utf8(text, length));
        }
    }
    if (FAILED(result)) {
        // Malformed XML, a DTD, or nesting deeper than allowed.
        return XmlError("the document is not well-formed XML LDM accepts", result);
    }
    auto root = tree.TakeRoot();
    if (!root) {
        return XmlError("the document has no root element");
    }
    return std::move(*root);
}

}  // namespace ldm::platform

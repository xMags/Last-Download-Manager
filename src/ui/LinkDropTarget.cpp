#include "ui/LinkDropTarget.h"

#include "base/Utf.h"
#include "core/Url.h"

#include <shlobj.h>

#include <algorithm>
#include <atomic>

namespace ldm::ui {

namespace {

// Larger drops are not addresses anyone dragged on purpose.
constexpr size_t kMaxDroppedText = 1024 * 1024;

std::wstring ReadText(IDataObject* data, CLIPFORMAT format)
{
    FORMATETC request{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&request, &medium))) {
        return {};
    }
    std::wstring text;
    if (const auto* chars = static_cast<const wchar_t*>(::GlobalLock(medium.hGlobal)); chars != nullptr) {
        const size_t capacity = ::GlobalSize(medium.hGlobal) / sizeof(wchar_t);
        const size_t length = ::wcsnlen(chars, std::min(capacity, kMaxDroppedText));
        text.assign(chars, length);
        ::GlobalUnlock(medium.hGlobal);
    }
    ::ReleaseStgMedium(&medium);
    return text;
}

}  // namespace

std::vector<std::string> UrlsFromDataObject(IDataObject* data)
{
    static const auto urlFormat = static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_INETURLW));
    std::vector<std::string> urls = core::ExtractUrls(base::ToUtf8(ReadText(data, urlFormat)));
    if (urls.empty()) {
        urls = core::ExtractUrls(base::ToUtf8(ReadText(data, CF_UNICODETEXT)));
    }
    return urls;
}

// The OLE drop target. Reference counted as COM requires; LinkDropTarget keeps one reference
// while registered and clears the callbacks before releasing it, so a late call from OLE never
// reaches a destroyed window.
class LinkDropTarget::Handler final : public IDropTarget {
public:
    explicit Handler(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

    void Detach() { callbacks_ = Callbacks{}; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (id == IID_IUnknown || id == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override
    {
        accept_ = !UrlsFromDataObject(data).empty();
        *effect = accept_ ? DROPEFFECT_LINK : DROPEFFECT_NONE;
        SetHover(accept_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override
    {
        *effect = accept_ ? DROPEFFECT_LINK : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        SetHover(false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override
    {
        SetHover(false);
        std::vector<std::string> urls = UrlsFromDataObject(data);
        *effect = urls.empty() ? DROPEFFECT_NONE : DROPEFFECT_LINK;
        if (!urls.empty() && callbacks_.dropped) {
            callbacks_.dropped(std::move(urls));
        }
        return S_OK;
    }

private:
    void SetHover(bool hovering) const
    {
        if (callbacks_.hovering) {
            callbacks_.hovering(hovering);
        }
    }

    Callbacks callbacks_;
    std::atomic<ULONG> references_{1};
    bool accept_ = false;
};

LinkDropTarget::~LinkDropTarget()
{
    Revoke();
}

bool LinkDropTarget::Register(HWND window, Callbacks callbacks)
{
    Revoke();
    auto* handler = new Handler(std::move(callbacks));  // COM object; released through Release()
    if (FAILED(::RegisterDragDrop(window, handler))) {
        handler->Release();
        return false;
    }
    window_ = window;
    handler_ = handler;
    return true;
}

void LinkDropTarget::Revoke()
{
    if (handler_ == nullptr) {
        return;
    }
    ::RevokeDragDrop(window_);
    handler_->Detach();
    handler_->Release();
    handler_ = nullptr;
    window_ = nullptr;
}

}  // namespace ldm::ui

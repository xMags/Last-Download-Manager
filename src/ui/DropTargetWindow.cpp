#include "ui/DropTargetWindow.h"

#include "base/Utf.h"
#include "core/Url.h"
#include "resource.h"
#include "ui/Dpi.h"

#include <shellscalingapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace ldm::ui {

namespace {

constexpr wchar_t kClassName[] = L"LdmDropTarget";
constexpr int kSize96 = 56;
constexpr UINT kHideCommand = 1;
constexpr UINT kOpenCommand = 2;
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

// The OLE drop target. Reference counted as COM requires; the window keeps one reference while
// registered and clears `owner` before releasing it, so a late call never reaches a destroyed window.
class DropTargetWindow::DropHandler final : public IDropTarget {
public:
    explicit DropHandler(DropTargetWindow* owner) : owner_(owner) {}

    void Detach() { owner_ = nullptr; }

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
        if (!urls.empty() && owner_ != nullptr && owner_->callbacks_.dropped) {
            owner_->callbacks_.dropped(std::move(urls));
        }
        return S_OK;
    }

private:
    void SetHover(bool hovering)
    {
        if (owner_ != nullptr && owner_->window_ != nullptr) {
            owner_->hovering_ = hovering;
            ::InvalidateRect(owner_->window_, nullptr, FALSE);
        }
    }

    DropTargetWindow* owner_;
    std::atomic<ULONG> references_{1};
    bool accept_ = false;
};

DropTargetWindow::~DropTargetWindow()
{
    Destroy();
}

void DropTargetWindow::Create(const std::string& position, const Theme& theme, Callbacks callbacks)
{
    if (window_ != nullptr) {
        return;
    }
    theme_ = &theme;
    callbacks_ = std::move(callbacks);
    static const ATOM atom = [] {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.style = CS_DBLCLKS;
        windowClass.lpfnWndProc = &DropTargetWindow::WindowProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_SIZEALL);
        windowClass.lpszClassName = kClassName;
        return ::RegisterClassExW(&windowClass);
    }();
    (void)atom;

    POINT origin{};
    int x = 0;
    int y = 0;
    if (sscanf_s(position.c_str(), "%d,%d", &x, &y) == 2) {
        origin = POINT{x, y};
    } else {
        RECT work{};
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        origin = POINT{work.right - 160, work.top + 120};
    }
    // Keep the window on a monitor that still exists.
    const HMONITOR monitor = ::MonitorFromPoint(origin, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    ::GetMonitorInfoW(monitor, &info);
    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    ::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const int size = Scale(kSize96, dpiX);
    origin.x = std::clamp(origin.x, info.rcWork.left, info.rcWork.right - size);
    origin.y = std::clamp(origin.y, info.rcWork.top, info.rcWork.bottom - size);

    window_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kClassName, L"Last Download Manager drop target",
                                WS_POPUP, origin.x, origin.y, size, size, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        return;
    }
    ::SetLayeredWindowAttributes(window_, 0, 230, LWA_ALPHA);
    HICON icon = nullptr;
    ::LoadIconWithScaleDown(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_LDM), size * 2 / 3, size * 2 / 3, &icon);
    icon_.reset(icon);
    handler_ = new DropHandler(this);  // COM object; released through Release()
    if (FAILED(::RegisterDragDrop(window_, handler_))) {
        handler_->Detach();
        handler_->Release();
        handler_ = nullptr;
    }
    ::ShowWindow(window_, SW_SHOWNOACTIVATE);
}

void DropTargetWindow::Destroy()
{
    if (window_ == nullptr) {
        return;
    }
    if (handler_ != nullptr) {
        ::RevokeDragDrop(window_);
        handler_->Detach();
        handler_->Release();
        handler_ = nullptr;
    }
    const HWND window = window_;
    window_ = nullptr;
    ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    ::DestroyWindow(window);
}

LRESULT CALLBACK DropTargetWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<DropTargetWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));  // stored at WM_NCCREATE
    if (self == nullptr || (self->window_ != nullptr && self->window_ != window)) {
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
    if (self->window_ == nullptr) {
        self->window_ = window;
    }
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT DropTargetWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_NCHITTEST:
        // The whole window is a caption: dragging it moves it.
        return HTCAPTION;
    case WM_NCLBUTTONDBLCLK:
        if (callbacks_.openMain) {
            callbacks_.openMain();
        }
        return 0;
    case WM_NCRBUTTONUP: {
        UniqueMenu menu(::CreatePopupMenu());
        ::AppendMenuW(menu.get(), MF_STRING, kOpenCommand, L"&Open Last Download Manager");
        ::AppendMenuW(menu.get(), MF_STRING, kHideCommand, L"&Hide drop target");
        const UINT command = static_cast<UINT>(::TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                                  GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), window_, nullptr));
        if (command == kHideCommand && callbacks_.hideRequested) {
            callbacks_.hideRequested();  // may destroy this window
        } else if (command == kOpenCommand && callbacks_.openMain) {
            callbacks_.openMain();
        }
        return 0;
    }
    case WM_EXITSIZEMOVE: {
        RECT rect{};
        ::GetWindowRect(window_, &rect);
        char text[32] = {};
        std::snprintf(text, sizeof(text), "%ld,%ld", rect.left, rect.top);
        if (callbacks_.moved) {
            callbacks_.moved(text);
        }
        return 0;
    }
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return ::DefWindowProcW(window_, message, wParam, lParam);
    }
}

void DropTargetWindow::Paint()
{
    PAINTSTRUCT paint{};
    const HDC dc = ::BeginPaint(window_, &paint);
    RECT client{};
    ::GetClientRect(window_, &client);
    const Palette& colors = theme_->Colors();
    UniqueBrush background(::CreateSolidBrush(hovering_ ? colors.accent : colors.panel));
    UniqueBrush border(::CreateSolidBrush(colors.accent));
    ::FillRect(dc, &client, background.get());
    ::FrameRect(dc, &client, border.get());
    const int iconSize = (client.right - client.left) * 2 / 3;
    ::DrawIconEx(dc, (client.right - iconSize) / 2, (client.bottom - iconSize) / 2, icon_.get(), iconSize, iconSize, 0,
                 nullptr, DI_NORMAL);
    ::EndPaint(window_, &paint);
}

}  // namespace ldm::ui

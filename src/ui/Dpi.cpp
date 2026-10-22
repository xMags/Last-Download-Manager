#include "ui/Dpi.h"

#include <cwchar>
#include <string>

namespace ldm::ui {

UniqueFont CreateMessageFont(UINT dpi)
{
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        return UniqueFont(static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT)));
    }
    return UniqueFont(::CreateFontIndirectW(&metrics.lfMessageFont));
}

UniqueFont CreateFontFromSetting(const std::wstring& setting, UINT dpi)
{
    const size_t separator = setting.find(L';');
    if (setting.empty() || separator == std::wstring::npos || separator == 0) {
        return CreateMessageFont(dpi);
    }
    const std::wstring face = setting.substr(0, separator);
    const int points = std::wcstol(setting.c_str() + separator + 1, nullptr, 10);
    if (points < 6 || points > 48 || face.size() >= LF_FACESIZE) {
        return CreateMessageFont(dpi);
    }
    LOGFONTW font{};
    font.lfHeight = -::MulDiv(points, static_cast<int>(dpi), 72);
    font.lfWeight = FW_NORMAL;
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy_s(font.lfFaceName, face.c_str(), _TRUNCATE);
    return UniqueFont(::CreateFontIndirectW(&font));
}

}  // namespace ldm::ui

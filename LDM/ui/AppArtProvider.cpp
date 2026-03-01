#include "AppArtProvider.h"

#include <wx/icon.h>
#include <wx/image.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#pragma comment(lib, "Shell32.lib")
#endif

namespace {

#ifdef _WIN32
wxSize ResolveIconSize(const wxSize &requested, const wxArtClient &client) {
  wxSize size = requested;
  if (size.GetWidth() <= 0 || size.GetHeight() <= 0) {
    size = wxArtProvider::GetNativeSizeHint(client);
  }
  if (size.GetWidth() <= 0 || size.GetHeight() <= 0) {
    size = wxSize(24, 24);
  }
  return size;
}

SHSTOCKICONID MapArtToStockIcon(const wxArtID &id) {
  if (id == wxART_NEW) {
    return SIID_DOCNOASSOC;
  }
  if (id == wxART_GO_FORWARD) {
    return SIID_LINK;
  }
  if (id == wxART_CROSS_MARK) {
    return SIID_ERROR;
  }
  if (id == wxART_STOP) {
    return SIID_DELETE;
  }
  if (id == wxART_DELETE) {
    return SIID_RECYCLER;
  }
  if (id == wxART_FIND) {
    return SIID_FIND;
  }
  if (id == wxART_HELP_SETTINGS) {
    return SIID_SETTINGS;
  }
  if (id == wxART_EXECUTABLE_FILE) {
    return SIID_APPLICATION;
  }

  return SIID_INVALID;
}

wxBitmap LoadStockIcon(SHSTOCKICONID stockId, const wxSize &size) {
  if (stockId == SIID_INVALID) {
    return wxNullBitmap;
  }

  SHSTOCKICONINFO sii = {};
  sii.cbSize = sizeof(sii);

  HRESULT hr = SHGetStockIconInfo(stockId, SHGSI_ICON | SHGSI_LARGEICON, &sii);
  if (FAILED(hr) || !sii.hIcon) {
    return wxNullBitmap;
  }

  // Preserve alpha: HICON -> wxIcon -> wxBitmap.
  wxIcon icon;
  icon.CreateFromHICON(sii.hIcon);
  DestroyIcon(sii.hIcon);
  wxBitmap bmp(icon);
  if (!bmp.IsOk()) {
    return wxNullBitmap;
  }

  bmp = wxBitmap(
      bmp.ConvertToImage().Rescale(size.x, size.y, wxIMAGE_QUALITY_HIGH));
  return bmp;
}
#endif

} // namespace

wxBitmap AppArtProvider::CreateBitmap(const wxArtID &id, const wxArtClient &client,
                                      const wxSize &size) {
#ifdef _WIN32
  const SHSTOCKICONID stockId = MapArtToStockIcon(id);
  if (stockId != SIID_INVALID) {
    const wxSize iconSize = ResolveIconSize(size, client);
    return LoadStockIcon(stockId, iconSize);
  }
#endif

  return wxNullBitmap;
}

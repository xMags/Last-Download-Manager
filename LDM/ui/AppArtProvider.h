#pragma once

#include <wx/artprov.h>

class AppArtProvider final : public wxArtProvider {
public:
  AppArtProvider() = default;
  ~AppArtProvider() override = default;

protected:
  wxBitmap CreateBitmap(const wxArtID &id, const wxArtClient &client,
                        const wxSize &size) override;
};

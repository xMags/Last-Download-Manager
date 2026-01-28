#pragma once

#include "../core/Download.h"
#include <memory>
#include <vector>
#include <wx/listctrl.h>
#include <wx/wx.h>

// Context menu IDs
enum {
  ID_CTX_OPEN = wxID_HIGHEST + 100,
  ID_CTX_OPEN_FOLDER,
  ID_CTX_RESUME,
  ID_CTX_PAUSE,
  ID_CTX_STOP,
  ID_CTX_DELETE,
  ID_CTX_DELETE_WITH_FILE,
  ID_CTX_PROPERTIES
};

class DownloadsTable : public wxPanel {
public:
  DownloadsTable(wxWindow *parent);
  ~DownloadsTable() = default;

  // Download management
  void AddDownload(std::shared_ptr<Download> download);
  void RemoveDownload(int downloadId);
  void UpdateDownload(int downloadId);
  void RefreshAll();

  // Category filtering
  void FilterByCategory(const wxString &category);
  void ClearFilter();

  // Selection
  int GetSelectedDownloadId() const;
  std::vector<int> GetSelectedDownloadIds() const;

  // Get selected download
  std::shared_ptr<Download> GetSelectedDownload() const;

private:
  wxListCtrl *m_listCtrl;
  std::vector<std::shared_ptr<Download>> m_downloads;
  std::vector<std::shared_ptr<Download>>
      m_filteredDownloads;  // Visible downloads after filtering
  wxString m_currentFilter; // Current category filter
  long m_contextMenuIndex;  // Index of right-clicked item

  void CreateColumns();
  void UpdateRow(long row, std::shared_ptr<Download> download);
  void ApplyFilter(); // Apply current filter to downloads
  wxString FormatFileSize(int64_t bytes) const;
  wxString FormatSpeed(double bytesPerSecond) const;
  wxString FormatTime(int seconds) const;

  // Event handlers
  void OnItemSelected(wxListEvent &event);
  void OnItemActivated(wxListEvent &event);
  void OnItemRightClick(wxListEvent &event);
  void OnColumnClick(wxListEvent &event);

  // Context menu handlers
  void OnContextOpen(wxCommandEvent &event);
  void OnContextOpenFolder(wxCommandEvent &event);
  void OnContextResume(wxCommandEvent &event);
  void OnContextPause(wxCommandEvent &event);
  void OnContextDelete(wxCommandEvent &event);
  void OnContextDeleteWithFile(wxCommandEvent &event);

  wxDECLARE_EVENT_TABLE();
};

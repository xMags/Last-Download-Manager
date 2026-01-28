#include "DownloadsTable.h"
#include "../core/DownloadManager.h"
#include "../utils/ThemeManager.h"
#include <shellapi.h>
#include <wx/artprov.h>


wxBEGIN_EVENT_TABLE(DownloadsTable, wxPanel) EVT_LIST_ITEM_SELECTED(
    wxID_ANY, DownloadsTable::OnItemSelected)
    EVT_LIST_ITEM_ACTIVATED(wxID_ANY, DownloadsTable::OnItemActivated)
        EVT_LIST_ITEM_RIGHT_CLICK(wxID_ANY, DownloadsTable::OnItemRightClick)
            EVT_LIST_COL_CLICK(wxID_ANY, DownloadsTable::OnColumnClick)
                EVT_MENU(ID_CTX_OPEN, DownloadsTable::OnContextOpen)
                    EVT_MENU(ID_CTX_OPEN_FOLDER,
                             DownloadsTable::OnContextOpenFolder)
                        EVT_MENU(ID_CTX_RESUME, DownloadsTable::OnContextResume)
                            EVT_MENU(ID_CTX_PAUSE,
                                     DownloadsTable::OnContextPause)
                                EVT_MENU(ID_CTX_DELETE,
                                         DownloadsTable::OnContextDelete)
                                    EVT_MENU(
                                        ID_CTX_DELETE_WITH_FILE,
                                        DownloadsTable::OnContextDeleteWithFile)
                                        wxEND_EVENT_TABLE()

                                            DownloadsTable::DownloadsTable(
                                                wxWindow *parent)
    : wxPanel(parent, wxID_ANY), m_contextMenuIndex(-1) {
  wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

  // Create list control
  m_listCtrl =
      new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_VRULES | wxLC_HRULES);

  CreateColumns();

  sizer->Add(m_listCtrl, 1, wxEXPAND);
  SetSizer(sizer);

  // Set initial colors
  ThemeManager::GetInstance().ApplyTheme(this);
}

void DownloadsTable::CreateColumns() {
  // Create columns
  m_listCtrl->InsertColumn(0, "File Name", wxLIST_FORMAT_LEFT, 250);
  m_listCtrl->InsertColumn(1, "Size", wxLIST_FORMAT_RIGHT, 80);
  m_listCtrl->InsertColumn(2, "Progress", wxLIST_FORMAT_RIGHT, 70);
  m_listCtrl->InsertColumn(3, "Status", wxLIST_FORMAT_LEFT, 100);
  m_listCtrl->InsertColumn(4, "Time left", wxLIST_FORMAT_RIGHT, 80);
  m_listCtrl->InsertColumn(5, "Transfer rate", wxLIST_FORMAT_RIGHT, 100);
  m_listCtrl->InsertColumn(6, "Last Try", wxLIST_FORMAT_LEFT, 120);
}

void DownloadsTable::AddDownload(std::shared_ptr<Download> download) {
  m_downloads.push_back(download);

  // Re-apply filter to show the new download if it matches current filter
  ApplyFilter();
}

void DownloadsTable::RemoveDownload(int downloadId) {
  for (size_t i = 0; i < m_downloads.size(); ++i) {
    if (m_downloads[i]->GetId() == downloadId) {
      m_downloads.erase(m_downloads.begin() + i);
      break;
    }
  }
  // Re-apply filter to update display
  ApplyFilter();
}

void DownloadsTable::UpdateDownload(int downloadId) {
  // Find the download in the filtered list and update it
  for (size_t i = 0; i < m_filteredDownloads.size(); ++i) {
    if (m_filteredDownloads[i]->GetId() == downloadId) {
      UpdateRow(static_cast<long>(i), m_filteredDownloads[i]);
      break;
    }
  }
}

void DownloadsTable::RefreshAll() { ApplyFilter(); }

void DownloadsTable::FilterByCategory(const wxString &category) {
  m_currentFilter = category;
  ApplyFilter();
}

void DownloadsTable::ClearFilter() {
  m_currentFilter = "";
  ApplyFilter();
}

void DownloadsTable::ApplyFilter() {
  m_listCtrl->DeleteAllItems();
  m_filteredDownloads.clear();

  for (const auto &download : m_downloads) {
    bool matches = false;

    if (m_currentFilter.IsEmpty() || m_currentFilter == "All Downloads") {
      // Show all downloads
      matches = true;
    } else if (m_currentFilter == "Finished") {
      // Show only completed downloads
      matches = (download->GetStatus() == DownloadStatus::Completed);
    } else if (m_currentFilter == "Unfinished") {
      // Show non-completed downloads
      matches = (download->GetStatus() != DownloadStatus::Completed);
    } else {
      // Filter by category name (Compressed, Documents, Music, Programs, Video)
      // Remove any count suffix like " (5)" from the filter
      wxString categoryName = m_currentFilter;
      int parenPos = categoryName.Find('(');
      if (parenPos != wxNOT_FOUND) {
        categoryName = categoryName.Left(parenPos).Trim();
      }
      matches = (download->GetCategory() == categoryName.ToStdString());
    }

    if (matches) {
      m_filteredDownloads.push_back(download);
      long index = m_listCtrl->InsertItem(m_listCtrl->GetItemCount(),
                                          download->GetFilename());
      UpdateRow(index, download);
    }
  }
}

void DownloadsTable::UpdateRow(long row, std::shared_ptr<Download> download) {
  m_listCtrl->SetItem(row, 0, download->GetFilename());
  m_listCtrl->SetItem(row, 1, FormatFileSize(download->GetTotalSize()));

  // Calculate and display progress percentage
  int progress = download->GetProgress();
  wxString progressStr;
  if (progress >= 0) {
    progressStr = wxString::Format("%d%%", progress);
  } else {
    progressStr = "-";
  }
  m_listCtrl->SetItem(row, 2, progressStr);

  m_listCtrl->SetItem(row, 3, download->GetStatusString());
  m_listCtrl->SetItem(row, 4, FormatTime(download->GetTimeRemaining()));
  m_listCtrl->SetItem(row, 5, FormatSpeed(download->GetSpeed()));
  m_listCtrl->SetItem(row, 6, download->GetLastTryTime());

  // Set row color based on status
  // Set row color based on status
  wxColour bgColor;
  DownloadStatus status = download->GetStatus();

  if (ThemeManager::GetInstance().IsDarkMode()) {
    // In dark mode, row background is control background, but we might want
    // slight tint Actually, simple list control usually handles selection, but
    // for specific rows we use:
    bgColor = ThemeManager::GetInstance().GetStatusColor(status);
  } else {
    bgColor = ThemeManager::GetInstance().GetStatusColor(status);
  }

  m_listCtrl->SetItemBackgroundColour(row, bgColor);

  // Set text color if needed (e.g. for dark mode readability)
  m_listCtrl->SetItemTextColour(
      row, ThemeManager::GetInstance().GetForegroundColor());
}

wxString DownloadsTable::FormatFileSize(int64_t bytes) const {
  if (bytes < 0)
    return "Unknown";
  if (bytes == 0)
    return "0 B";

  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  int unitIndex = 0;
  double size = static_cast<double>(bytes);

  while (size >= 1024 && unitIndex < 4) {
    size /= 1024;
    unitIndex++;
  }

  return wxString::Format("%.1f %s", size, units[unitIndex]);
}

wxString DownloadsTable::FormatSpeed(double bytesPerSecond) const {
  if (bytesPerSecond <= 0)
    return "0 KB/s";

  if (bytesPerSecond < 1024) {
    return wxString::Format("%.0f B/s", bytesPerSecond);
  } else if (bytesPerSecond < 1024 * 1024) {
    return wxString::Format("%.1f KB/s", bytesPerSecond / 1024);
  } else {
    return wxString::Format("%.2f MB/s", bytesPerSecond / (1024 * 1024));
  }
}

wxString DownloadsTable::FormatTime(int seconds) const {
  if (seconds < 0)
    return "Unknown";
  if (seconds == 0)
    return "-";

  int hours = seconds / 3600;
  int minutes = (seconds % 3600) / 60;
  int secs = seconds % 60;

  if (hours > 0) {
    return wxString::Format("%d:%02d:%02d", hours, minutes, secs);
  } else {
    return wxString::Format("%d:%02d", minutes, secs);
  }
}

int DownloadsTable::GetSelectedDownloadId() const {
  long selectedIndex =
      m_listCtrl->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (selectedIndex >= 0 &&
      selectedIndex < static_cast<long>(m_filteredDownloads.size())) {
    return m_filteredDownloads[selectedIndex]->GetId();
  }
  return -1;
}

std::shared_ptr<Download> DownloadsTable::GetSelectedDownload() const {
  long selectedIndex =
      m_listCtrl->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (selectedIndex >= 0 &&
      selectedIndex < static_cast<long>(m_filteredDownloads.size())) {
    return m_filteredDownloads[selectedIndex];
  }
  return nullptr;
}

std::vector<int> DownloadsTable::GetSelectedDownloadIds() const {
  std::vector<int> ids;
  long selectedIndex = -1;

  while ((selectedIndex = m_listCtrl->GetNextItem(
              selectedIndex, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1) {
    if (selectedIndex < static_cast<long>(m_filteredDownloads.size())) {
      ids.push_back(m_filteredDownloads[selectedIndex]->GetId());
    }
  }

  return ids;
}

void DownloadsTable::OnItemSelected(wxListEvent &event) { event.Skip(); }

void DownloadsTable::OnItemActivated(wxListEvent &event) {
  // Double-click: Open file
  long index = event.GetIndex();
  if (index >= 0 && index < static_cast<long>(m_filteredDownloads.size())) {
    auto download = m_filteredDownloads[index];
    if (download->GetStatus() == DownloadStatus::Completed) {
      std::string filePath =
          download->GetSavePath() + "\\" + download->GetFilename();
      ShellExecuteA(NULL, "open", filePath.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
  }
  event.Skip();
}

void DownloadsTable::OnItemRightClick(wxListEvent &event) {
  m_contextMenuIndex = event.GetIndex();

  // Create context menu
  wxMenu contextMenu;
  contextMenu.Append(ID_CTX_OPEN, "Open");
  contextMenu.Append(ID_CTX_OPEN_FOLDER, "Open Folder");
  contextMenu.AppendSeparator();
  contextMenu.Append(ID_CTX_RESUME, "Resume");
  contextMenu.Append(ID_CTX_PAUSE, "Pause");
  contextMenu.AppendSeparator();
  contextMenu.Append(ID_CTX_DELETE, "Delete");
  contextMenu.Append(ID_CTX_DELETE_WITH_FILE, "Delete with File");

  PopupMenu(&contextMenu);
}

void DownloadsTable::OnColumnClick(wxListEvent &event) { event.Skip(); }

void DownloadsTable::OnContextOpen(wxCommandEvent &event) {
  if (m_contextMenuIndex >= 0 &&
      m_contextMenuIndex < static_cast<long>(m_filteredDownloads.size())) {
    auto download = m_filteredDownloads[m_contextMenuIndex];
    std::string filePath =
        download->GetSavePath() + "\\" + download->GetFilename();
    ShellExecuteA(NULL, "open", filePath.c_str(), NULL, NULL, SW_SHOWNORMAL);
  }
}

void DownloadsTable::OnContextOpenFolder(wxCommandEvent &event) {
  if (m_contextMenuIndex >= 0 &&
      m_contextMenuIndex < static_cast<long>(m_filteredDownloads.size())) {
    auto download = m_filteredDownloads[m_contextMenuIndex];
    std::string filePath =
        download->GetSavePath() + "\\" + download->GetFilename();

    // Use /select to highlight the file in Explorer
    std::string explorerCmd = "/select,\"" + filePath + "\"";
    ShellExecuteA(NULL, "open", "explorer.exe", explorerCmd.c_str(), NULL,
                  SW_SHOWNORMAL);
  }
}

void DownloadsTable::OnContextResume(wxCommandEvent &event) {
  if (m_contextMenuIndex >= 0 &&
      m_contextMenuIndex < static_cast<long>(m_filteredDownloads.size())) {
    int downloadId = m_filteredDownloads[m_contextMenuIndex]->GetId();
    DownloadManager::GetInstance().ResumeDownload(downloadId);
  }
}

void DownloadsTable::OnContextPause(wxCommandEvent &event) {
  if (m_contextMenuIndex >= 0 &&
      m_contextMenuIndex < static_cast<long>(m_filteredDownloads.size())) {
    int downloadId = m_filteredDownloads[m_contextMenuIndex]->GetId();
    DownloadManager::GetInstance().PauseDownload(downloadId);
  }
}

void DownloadsTable::OnContextDelete(wxCommandEvent &event) {
  if (m_contextMenuIndex >= 0 &&
      m_contextMenuIndex < static_cast<long>(m_filteredDownloads.size())) {

    auto download = m_filteredDownloads[m_contextMenuIndex];

    // Create a custom dialog with checkbox
    wxDialog dlg(this, wxID_ANY, "Delete Download", wxDefaultPosition,
                 wxDefaultSize, wxDEFAULT_DIALOG_STYLE);
    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

    // Message
    wxStaticText *msg = new wxStaticText(
        &dlg, wxID_ANY,
        wxString::Format("Remove '%s' from list?", download->GetFilename()));
    mainSizer->Add(msg, 0, wxALL, 15);

    // Checkbox for delete file
    wxCheckBox *deleteFileCheck =
        new wxCheckBox(&dlg, wxID_ANY, "Also delete the file from disk");
    mainSizer->Add(deleteFileCheck, 0, wxLEFT | wxRIGHT | wxBOTTOM, 15);

    // Buttons
    wxStdDialogButtonSizer *btnSizer = new wxStdDialogButtonSizer();
    wxButton *btnOk = new wxButton(&dlg, wxID_OK, "Delete");
    wxButton *btnCancel = new wxButton(&dlg, wxID_CANCEL, "Cancel");
    btnSizer->AddButton(btnOk);
    btnSizer->AddButton(btnCancel);
    btnSizer->Realize();
    mainSizer->Add(btnSizer, 0, wxALIGN_RIGHT | wxALL, 15);

    dlg.SetSizer(mainSizer);
    mainSizer->Fit(&dlg);
    dlg.CenterOnParent();

    if (dlg.ShowModal() == wxID_OK) {
      bool deleteFile = deleteFileCheck->GetValue();
      int downloadId = download->GetId();
      DownloadManager::GetInstance().RemoveDownload(downloadId, deleteFile);
      RemoveDownload(downloadId);
    }
  }
}

void DownloadsTable::OnContextDeleteWithFile(wxCommandEvent &event) {
  // Same as OnContextDelete but with checkbox pre-checked
  if (m_contextMenuIndex >= 0 &&
      m_contextMenuIndex < static_cast<long>(m_filteredDownloads.size())) {
    int downloadId = m_filteredDownloads[m_contextMenuIndex]->GetId();

    int result = wxMessageBox(
        "Are you sure you want to delete this download AND the file?",
        "Confirm Delete", wxYES_NO | wxICON_WARNING);
    if (result == wxYES) {
      DownloadManager::GetInstance().RemoveDownload(downloadId, true);
      RemoveDownload(downloadId);
    }
  }
}

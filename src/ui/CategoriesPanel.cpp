#include "CategoriesPanel.h"
#include "../utils/ThemeManager.h"
#include <wx/artprov.h>

wxBEGIN_EVENT_TABLE(CategoriesPanel, wxPanel)
    EVT_TREE_SEL_CHANGED(wxID_ANY, CategoriesPanel::OnSelectionChanged)
        EVT_TREE_ITEM_RIGHT_CLICK(wxID_ANY, CategoriesPanel::OnItemRightClick)
            wxEND_EVENT_TABLE()

                CategoriesPanel::CategoriesPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY) {
  // Create sizer
  wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

  // Create header label
  wxStaticText *headerLabel = new wxStaticText(this, wxID_ANY, "Categories");
  headerLabel->SetFont(headerLabel->GetFont().Bold());
  sizer->Add(headerLabel, 0, wxALL | wxEXPAND, 5);

  // Create tree control
  m_treeCtrl = new wxTreeCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                              wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT |
                                  wxTR_SINGLE | wxTR_HIDE_ROOT);

  // Create image list and categories
  CreateImageList();
  CreateCategories();

  sizer->Add(m_treeCtrl, 1, wxEXPAND | wxALL, 2);

  SetSizer(sizer);

  // Set initial colors
  ThemeManager::GetInstance().ApplyTheme(this);
}

void CategoriesPanel::CreateImageList() {
  m_imageList = new wxImageList(16, 16, true);

  // Add icons using wxArtProvider (placeholder icons)
  // In production, you would use custom icons
  m_imageList->Add(wxArtProvider::GetBitmap(wxART_FOLDER, wxART_LIST,
                                            wxSize(16, 16))); // 0: folder
  m_imageList->Add(wxArtProvider::GetBitmap(
      wxART_HARDDISK, wxART_LIST, wxSize(16, 16))); // 1: all downloads
  m_imageList->Add(wxArtProvider::GetBitmap(wxART_FILE_SAVE, wxART_LIST,
                                            wxSize(16, 16))); // 2: compressed
  m_imageList->Add(wxArtProvider::GetBitmap(wxART_NORMAL_FILE, wxART_LIST,
                                            wxSize(16, 16))); // 3: documents
  m_imageList->Add(wxArtProvider::GetBitmap(wxART_CDROM, wxART_LIST,
                                            wxSize(16, 16))); // 4: music
  m_imageList->Add(wxArtProvider::GetBitmap(wxART_EXECUTABLE_FILE, wxART_LIST,
                                            wxSize(16, 16))); // 5: programs
  m_imageList->Add(wxArtProvider::GetBitmap(wxART_MISSING_IMAGE, wxART_LIST,
                                            wxSize(16, 16))); // 6: video
  m_imageList->Add(wxArtProvider::GetBitmap(wxART_WARNING, wxART_LIST,
                                            wxSize(16, 16))); // 7: unfinished
  m_imageList->Add(wxArtProvider::GetBitmap(wxART_TICK_MARK, wxART_LIST,
                                            wxSize(16, 16))); // 8: finished
  m_imageList->Add(wxArtProvider::GetBitmap(wxART_FIND, wxART_LIST,
                                            wxSize(16, 16))); // 9: grabber
  m_imageList->Add(wxArtProvider::GetBitmap(wxART_LIST_VIEW, wxART_LIST,
                                            wxSize(16, 16))); // 10: queues

  m_treeCtrl->SetImageList(m_imageList);
}

void CategoriesPanel::CreateCategories() {
  // Create root (hidden)
  m_rootId = m_treeCtrl->AddRoot("Root");

  // Add main categories
  m_allDownloadsId = m_treeCtrl->AppendItem(m_rootId, "All Downloads", 1, 1);

  // Category sub-items under All Downloads
  m_compressedId = m_treeCtrl->AppendItem(m_allDownloadsId, "Compressed", 2, 2);
  m_documentsId = m_treeCtrl->AppendItem(m_allDownloadsId, "Documents", 3, 3);
  m_musicId = m_treeCtrl->AppendItem(m_allDownloadsId, "Music", 4, 4);
  m_programsId = m_treeCtrl->AppendItem(m_allDownloadsId, "Programs", 5, 5);
  m_videoId = m_treeCtrl->AppendItem(m_allDownloadsId, "Video", 6, 6);

  // Status categories
  m_unfinishedId = m_treeCtrl->AppendItem(m_rootId, "Unfinished", 7, 7);
  m_finishedId = m_treeCtrl->AppendItem(m_rootId, "Finished", 8, 8);

  // Other categories
  m_grabberProjectsId =
      m_treeCtrl->AppendItem(m_rootId, "Grabber projects", 9, 9);
  m_queuesId = m_treeCtrl->AppendItem(m_rootId, "Queues", 10, 10);

  // Expand All Downloads by default
  m_treeCtrl->Expand(m_allDownloadsId);

  // Select All Downloads by default
  m_treeCtrl->SelectItem(m_allDownloadsId);
}

wxString CategoriesPanel::GetSelectedCategory() const {
  wxTreeItemId selectedId = m_treeCtrl->GetSelection();

  if (!selectedId.IsOk() || selectedId == m_rootId) {
    return "All Downloads";
  }

  return m_treeCtrl->GetItemText(selectedId);
}

void CategoriesPanel::UpdateCategoryCount(const wxString &category, int count) {
  wxTreeItemId itemId;

  if (category == "All Downloads")
    itemId = m_allDownloadsId;
  else if (category == "Compressed")
    itemId = m_compressedId;
  else if (category == "Documents")
    itemId = m_documentsId;
  else if (category == "Music")
    itemId = m_musicId;
  else if (category == "Programs")
    itemId = m_programsId;
  else if (category == "Video")
    itemId = m_videoId;
  else if (category == "Unfinished")
    itemId = m_unfinishedId;
  else if (category == "Finished")
    itemId = m_finishedId;
  else
    return;

  if (itemId.IsOk()) {
    wxString text = category;
    if (count > 0) {
      text += wxString::Format(" (%d)", count);
    }
    m_treeCtrl->SetItemText(itemId, text);
  }
}

void CategoriesPanel::OnSelectionChanged(wxTreeEvent &event) {
  wxTreeItemId itemId = event.GetItem();
  if (itemId.IsOk()) {
    wxString category = m_treeCtrl->GetItemText(itemId);
    // TODO: Notify parent to filter downloads by this category
  }
  event.Skip();
}

void CategoriesPanel::OnItemRightClick(wxTreeEvent &event) {
  wxTreeItemId itemId = event.GetItem();
  if (itemId.IsOk()) {
    m_treeCtrl->SelectItem(itemId);

    // Create context menu
    wxMenu contextMenu;
    contextMenu.Append(wxID_ANY, "Open Folder");
    contextMenu.AppendSeparator();
    contextMenu.Append(wxID_ANY, "New Category...");
    contextMenu.Append(wxID_ANY, "Rename...");
    contextMenu.Append(wxID_ANY, "Delete");

    PopupMenu(&contextMenu);
  }
}

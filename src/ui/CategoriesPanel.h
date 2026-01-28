#pragma once

#include <wx/imaglist.h>
#include <wx/treectrl.h>
#include <wx/wx.h>


class CategoriesPanel : public wxPanel {
public:
  CategoriesPanel(wxWindow *parent);
  ~CategoriesPanel() = default;

  // Get selected category
  wxString GetSelectedCategory() const;

  // Update download counts
  void UpdateCategoryCount(const wxString &category, int count);

private:
  wxTreeCtrl *m_treeCtrl;
  wxImageList *m_imageList;

  // Tree item IDs
  wxTreeItemId m_rootId;
  wxTreeItemId m_allDownloadsId;
  wxTreeItemId m_compressedId;
  wxTreeItemId m_documentsId;
  wxTreeItemId m_musicId;
  wxTreeItemId m_programsId;
  wxTreeItemId m_videoId;
  wxTreeItemId m_unfinishedId;
  wxTreeItemId m_finishedId;
  wxTreeItemId m_grabberProjectsId;
  wxTreeItemId m_queuesId;

  void CreateImageList();
  void CreateCategories();

  // Event handlers
  void OnSelectionChanged(wxTreeEvent &event);
  void OnItemRightClick(wxTreeEvent &event);

  wxDECLARE_EVENT_TABLE();
};

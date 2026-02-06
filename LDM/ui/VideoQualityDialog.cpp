#include "VideoQualityDialog.h"
#include <wx/statline.h>
#include <sstream>
#include <iomanip>

enum {
  ID_FORMAT_LIST = wxID_HIGHEST + 1,
  ID_REMEMBER_CHECK
};

wxBEGIN_EVENT_TABLE(VideoQualityDialog, wxDialog)
  EVT_BUTTON(wxID_OK, VideoQualityDialog::OnOK)
  EVT_BUTTON(wxID_CANCEL, VideoQualityDialog::OnCancel)
  EVT_LIST_ITEM_SELECTED(ID_FORMAT_LIST, VideoQualityDialog::OnFormatSelected)
  EVT_LIST_ITEM_ACTIVATED(ID_FORMAT_LIST, VideoQualityDialog::OnFormatActivated)
wxEND_EVENT_TABLE()

VideoQualityDialog::VideoQualityDialog(wxWindow *parent, const std::string &url, const std::string &title)
    : wxDialog(parent, wxID_ANY, "Select Video Quality", wxDefaultPosition, wxSize(500, 450),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_url(url), m_videoTitle(title), m_rememberChoice(false) {

  wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

  // Title label
  m_titleLabel = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(title));
  m_titleLabel->Wrap(460);
  wxFont titleFont = m_titleLabel->GetFont();
  titleFont.SetWeight(wxFONTWEIGHT_BOLD);
  m_titleLabel->SetFont(titleFont);
  mainSizer->Add(m_titleLabel, 0, wxALL | wxEXPAND, 10);

  // Status label
  m_statusLabel = new wxStaticText(this, wxID_ANY, "Loading available formats...");
  mainSizer->Add(m_statusLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

  // Format list
  m_formatList = new wxListCtrl(this, ID_FORMAT_LIST, wxDefaultPosition, wxSize(-1, 250),
                                 wxLC_REPORT | wxLC_SINGLE_SEL);
  m_formatList->InsertColumn(0, "Quality", wxLIST_FORMAT_LEFT, 200);
  m_formatList->InsertColumn(1, "Format", wxLIST_FORMAT_LEFT, 80);
  m_formatList->InsertColumn(2, "Size", wxLIST_FORMAT_LEFT, 100);
  mainSizer->Add(m_formatList, 1, wxLEFT | wxRIGHT | wxEXPAND, 10);

  // Remember checkbox
  m_rememberCheck = new wxCheckBox(this, ID_REMEMBER_CHECK, "Remember my choice for this session");
  mainSizer->Add(m_rememberCheck, 0, wxALL, 10);

  // Separator
  mainSizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

  // Buttons
  wxBoxSizer *buttonSizer = new wxBoxSizer(wxHORIZONTAL);
  buttonSizer->AddStretchSpacer();
  buttonSizer->Add(new wxButton(this, wxID_CANCEL, "Cancel"), 0, wxALL, 5);
  buttonSizer->Add(new wxButton(this, wxID_OK, "Download"), 0, wxALL, 5);
  mainSizer->Add(buttonSizer, 0, wxEXPAND | wxALL, 5);

  SetSizer(mainSizer);
  Centre();

  // Populate formats in background
  PopulateFormats();
}

void VideoQualityDialog::PopulateFormats() {
  // Get formats (this is a blocking call but usually fast)
  YtDlpManager &ytdlp = YtDlpManager::GetInstance();
  m_formats = ytdlp.GetAvailableFormats(m_url);

  if (m_formats.empty()) {
    m_statusLabel->SetLabel("No formats available. Using default quality.");
    m_selectedFormat = "";  // Will use default
    return;
  }

  // Check if ffmpeg is available and show appropriate message
  if (ytdlp.IsFfmpegAvailable()) {
    m_statusLabel->SetLabel("Select a quality option:");
  } else {
    m_statusLabel->SetLabel("Select quality (install ffmpeg for 1080p+ options):");
  }

  // Populate list
  for (size_t i = 0; i < m_formats.size(); ++i) {
    const VideoFormat &fmt = m_formats[i];

    long index = m_formatList->InsertItem(static_cast<long>(i), wxString::FromUTF8(fmt.note));
    m_formatList->SetItem(index, 1, wxString::FromUTF8(fmt.ext));

    // Format file size
    wxString sizeStr;
    if (fmt.filesize > 0) {
      if (fmt.filesize > 1024 * 1024 * 1024) {
        sizeStr = wxString::Format("%.1f GB", fmt.filesize / (1024.0 * 1024.0 * 1024.0));
      } else if (fmt.filesize > 1024 * 1024) {
        sizeStr = wxString::Format("%.1f MB", fmt.filesize / (1024.0 * 1024.0));
      } else {
        sizeStr = wxString::Format("%.1f KB", fmt.filesize / 1024.0);
      }
    } else {
      sizeStr = "Unknown";
    }
    m_formatList->SetItem(index, 2, sizeStr);
  }

  // Select first item (Best Quality) by default
  if (m_formatList->GetItemCount() > 0) {
    m_formatList->SetItemState(0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    m_selectedFormat = m_formats[0].formatId;
  }
}

void VideoQualityDialog::OnFormatSelected(wxListEvent &event) {
  long index = event.GetIndex();
  if (index >= 0 && index < static_cast<long>(m_formats.size())) {
    m_selectedFormat = m_formats[index].formatId;
  }
}

void VideoQualityDialog::OnFormatActivated(wxListEvent &event) {
  // Double-click starts download
  OnFormatSelected(event);
  EndModal(wxID_OK);
}

void VideoQualityDialog::OnOK(wxCommandEvent &event) {
  m_rememberChoice = m_rememberCheck->GetValue();

  // Ensure we have a selection
  long selected = m_formatList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (selected >= 0 && selected < static_cast<long>(m_formats.size())) {
    m_selectedFormat = m_formats[selected].formatId;
  }

  EndModal(wxID_OK);
}

void VideoQualityDialog::OnCancel(wxCommandEvent &event) {
  m_selectedFormat = "";
  EndModal(wxID_CANCEL);
}

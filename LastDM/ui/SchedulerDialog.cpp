#include "SchedulerDialog.h"
#include "../core/DownloadManager.h"
#include "../utils/ThemeManager.h"
#include <wx/datectrl.h>
#include <wx/timectrl.h>

wxBEGIN_EVENT_TABLE(SchedulerDialog, wxDialog)
    EVT_BUTTON(wxID_OK, SchedulerDialog::OnOK)
        EVT_BUTTON(wxID_CANCEL, SchedulerDialog::OnCancel)
            EVT_BUTTON(wxID_APPLY, SchedulerDialog::OnApply) wxEND_EVENT_TABLE()

                SchedulerDialog::SchedulerDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, "Scheduler", wxDefaultPosition,
               wxSize(450, 400)) {
  InitUI();

  // Apply current theme
  ThemeManager::GetInstance().ApplyTheme(this);

  CenterOnParent();
}

void SchedulerDialog::InitUI() {
  wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

  // Notebook for tabs (optional)
  // For now, simpler single page layout

  // --- Schedule Group ---
  wxStaticBoxSizer *scheduleSizer =
      new wxStaticBoxSizer(wxVERTICAL, this, "Schedule");

  // Start Time
  wxBoxSizer *startSizer = new wxBoxSizer(wxHORIZONTAL);
  m_chkStartTime = new wxCheckBox(this, wxID_ANY, "Start download at:");
  m_datePickerStart = new wxDatePickerCtrl(this, wxID_ANY);
  m_timePickerStart = new wxTimePickerCtrl(this, wxID_ANY);
  m_chkStartTime->SetValue(false); // Default

  startSizer->Add(m_chkStartTime, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
  startSizer->Add(m_datePickerStart, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
  startSizer->Add(m_timePickerStart, 0, wxALIGN_CENTER_VERTICAL);
  scheduleSizer->Add(startSizer, 0, wxALL | wxEXPAND, 5);

  // Stop Time
  wxBoxSizer *stopSizer = new wxBoxSizer(wxHORIZONTAL);
  m_chkStopTime = new wxCheckBox(this, wxID_ANY, "Stop download at: ");
  m_datePickerStop = new wxDatePickerCtrl(this, wxID_ANY);
  m_timePickerStop = new wxTimePickerCtrl(this, wxID_ANY);
  m_chkStopTime->SetValue(false);

  stopSizer->Add(m_chkStopTime, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
  stopSizer->Add(m_datePickerStop, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
  stopSizer->Add(m_timePickerStop, 0, wxALIGN_CENTER_VERTICAL);
  scheduleSizer->Add(stopSizer, 0, wxALL | wxEXPAND, 5);

  mainSizer->Add(scheduleSizer, 0, wxALL | wxEXPAND, 10);

  // --- Queue Settings Group ---
  wxStaticBoxSizer *queueSizer =
      new wxStaticBoxSizer(wxVERTICAL, this, "Queue Settings");

  wxBoxSizer *maxDlSizer = new wxBoxSizer(wxHORIZONTAL);
  maxDlSizer->Add(new wxStaticText(this, wxID_ANY, "Max concurrent downloads:"),
                  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
  m_spinMaxDownloads = new wxSpinCtrl(this, wxID_ANY);
  m_spinMaxDownloads->SetRange(1, 100);
  m_spinMaxDownloads->SetValue(4); // Default
  maxDlSizer->Add(m_spinMaxDownloads, 0, wxALIGN_CENTER_VERTICAL);
  queueSizer->Add(maxDlSizer, 0, wxALL | wxEXPAND, 5);

  mainSizer->Add(queueSizer, 0, wxALL | wxEXPAND, 10);

  // --- Completion Actions ---
  wxStaticBoxSizer *actionSizer =
      new wxStaticBoxSizer(wxVERTICAL, this, "On Completion");
  m_chkHangUp = new wxCheckBox(this, wxID_ANY, "Hang up modem when done");
  m_chkExit = new wxCheckBox(this, wxID_ANY, "Exit LastDM when done");
  m_chkShutdown = new wxCheckBox(this, wxID_ANY, "Turn off computer when done");

  actionSizer->Add(m_chkHangUp, 0, wxALL, 5);
  actionSizer->Add(m_chkExit, 0, wxALL, 5);
  actionSizer->Add(m_chkShutdown, 0, wxALL, 5);

  mainSizer->Add(actionSizer, 0, wxALL | wxEXPAND, 10);

  // --- Buttons ---
  wxStdDialogButtonSizer *btnSizer = new wxStdDialogButtonSizer();
  btnSizer->AddButton(new wxButton(this, wxID_OK));
  btnSizer->AddButton(new wxButton(this, wxID_CANCEL));
  btnSizer->AddButton(new wxButton(this, wxID_APPLY));
  btnSizer->Realize();

  mainSizer->Add(btnSizer, 0, wxALIGN_RIGHT | wxALL, 10);

  SetSizer(mainSizer);
  Layout();
}

bool SchedulerDialog::IsStartTimeEnabled() const {
  return m_chkStartTime->GetValue();
}
wxDateTime SchedulerDialog::GetStartTime() const {
  wxDateTime date = m_datePickerStart->GetValue();
  wxDateTime time = m_timePickerStart->GetValue();
  date.SetHour(time.GetHour());
  date.SetMinute(time.GetMinute());
  date.SetSecond(time.GetSecond());
  return date;
}

bool SchedulerDialog::IsStopTimeEnabled() const {
  return m_chkStopTime->GetValue();
}
wxDateTime SchedulerDialog::GetStopTime() const {
  wxDateTime date = m_datePickerStop->GetValue();
  wxDateTime time = m_timePickerStop->GetValue();
  date.SetHour(time.GetHour());
  date.SetMinute(time.GetMinute());
  date.SetSecond(time.GetSecond());
  return date;
}

int SchedulerDialog::GetMaxConcurrentDownloads() const {
  return m_spinMaxDownloads->GetValue();
}
bool SchedulerDialog::ShouldHangUpWhenDone() const {
  return m_chkHangUp->GetValue();
}
bool SchedulerDialog::ShouldExitWhenDone() const {
  return m_chkExit->GetValue();
}
bool SchedulerDialog::ShouldShutdownWhenDone() const {
  return m_chkShutdown->GetValue();
}

void SchedulerDialog::OnOK(wxCommandEvent &event) {
  // TODO: Save settings to SettingsManager or Apply directly
  EndModal(wxID_OK);
}

void SchedulerDialog::OnCancel(wxCommandEvent &event) { EndModal(wxID_CANCEL); }

void SchedulerDialog::OnApply(wxCommandEvent &event) {
  // TODO: Apply settings without closing
}

void SchedulerDialog::OnStartNow(wxCommandEvent &event) {
  // Start queue immediately
}

void SchedulerDialog::OnStopNow(wxCommandEvent &event) {
  // Stop queue immediately
}

#pragma once

#include <wx/datectrl.h>
#include <wx/dialog.h>
#include <wx/spinctrl.h>
#include <wx/timectrl.h>
#include <wx/wx.h>

class SchedulerDialog : public wxDialog {
public:
  SchedulerDialog(wxWindow *parent);
  ~SchedulerDialog() = default;

  // Getters for settings
  bool IsStartTimeEnabled() const;
  wxDateTime GetStartTime() const;

  bool IsStopTimeEnabled() const;
  wxDateTime GetStopTime() const;

  int GetMaxConcurrentDownloads() const;
  bool ShouldHangUpWhenDone() const;
  bool ShouldExitWhenDone() const;
  bool ShouldShutdownWhenDone() const;

private:
  void InitUI();

  // UI Controls
  wxCheckBox *m_chkStartTime;
  wxDatePickerCtrl *m_datePickerStart;
  wxTimePickerCtrl *m_timePickerStart;

  wxCheckBox *m_chkStopTime;
  wxDatePickerCtrl *m_datePickerStop;
  wxTimePickerCtrl *m_timePickerStop;

  wxSpinCtrl *m_spinMaxDownloads;

  wxCheckBox *m_chkHangUp;
  wxCheckBox *m_chkExit;
  wxCheckBox *m_chkShutdown;

  // Event handlers
  void OnOK(wxCommandEvent &event);
  void OnCancel(wxCommandEvent &event);
  void OnApply(wxCommandEvent &event);
  void OnStartNow(wxCommandEvent &event);
  void OnStopNow(wxCommandEvent &event);

  wxDECLARE_EVENT_TABLE();
};

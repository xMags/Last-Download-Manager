#pragma once

#include <chrono>
#include <deque>
#include <wx/dcbuffer.h>
#include <wx/wx.h>


// Number of data points to display in the graph
constexpr size_t SPEED_HISTORY_SIZE = 60;

class SpeedGraphPanel : public wxPanel {
public:
  SpeedGraphPanel(wxWindow *parent, wxWindowID id = wxID_ANY);
  virtual ~SpeedGraphPanel() = default;

  // Update the graph with current speed (bytes per second)
  void UpdateSpeed(double speedBps);

  // Clear all speed history
  void Clear();

  // Get the maximum speed recorded
  double GetMaxSpeed() const { return m_maxSpeed; }

private:
  // Speed history (newest at back)
  std::deque<double> m_speedHistory;

  // Maximum speed for scaling
  double m_maxSpeed = 0.0;

  // Colors
  wxColour m_bgColor{30, 30, 30};
  wxColour m_gridColor{60, 60, 60};
  wxColour m_lineColor{0, 200, 100};
  wxColour m_fillColor{0, 200, 100, 40};
  wxColour m_textColor{180, 180, 180};

  // Drawing helpers
  void OnPaint(wxPaintEvent &event);
  void OnSize(wxSizeEvent &event);
  void DrawGrid(wxDC &dc, const wxRect &rect);
  void DrawSpeedLine(wxDC &dc, const wxRect &rect);
  void DrawLabels(wxDC &dc, const wxRect &rect);

  // Format speed for display
  static wxString FormatSpeed(double speedBps);

  wxDECLARE_EVENT_TABLE();
};

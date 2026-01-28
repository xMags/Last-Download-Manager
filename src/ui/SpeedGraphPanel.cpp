#include "SpeedGraphPanel.h"
#include <algorithm>
#include <cmath>

wxBEGIN_EVENT_TABLE(SpeedGraphPanel, wxPanel)
    EVT_PAINT(SpeedGraphPanel::OnPaint) EVT_SIZE(SpeedGraphPanel::OnSize)
        wxEND_EVENT_TABLE()

            SpeedGraphPanel::SpeedGraphPanel(wxWindow *parent, wxWindowID id)
    : wxPanel(parent, id, wxDefaultPosition, wxSize(-1, 100)) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  SetMinSize(wxSize(200, 80));

  // Initialize with zeros
  m_speedHistory.resize(SPEED_HISTORY_SIZE, 0.0);
}

void SpeedGraphPanel::UpdateSpeed(double speedBps) {
  // Add new speed to history
  m_speedHistory.push_back(speedBps);

  // Remove oldest if we exceed size
  while (m_speedHistory.size() > SPEED_HISTORY_SIZE) {
    m_speedHistory.pop_front();
  }

  // Update max speed (with some decay to adapt to changes)
  double currentMax =
      *std::max_element(m_speedHistory.begin(), m_speedHistory.end());
  if (currentMax > m_maxSpeed) {
    m_maxSpeed = currentMax;
  } else {
    // Slowly decay max towards current max for better scaling
    m_maxSpeed = m_maxSpeed * 0.99 + currentMax * 0.01;
    if (m_maxSpeed < currentMax * 1.1) {
      m_maxSpeed = currentMax * 1.1;
    }
  }

  // Ensure minimum scale
  if (m_maxSpeed < 1024.0) {
    m_maxSpeed = 1024.0; // At least 1 KB/s
  }

  Refresh();
}

void SpeedGraphPanel::Clear() {
  m_speedHistory.clear();
  m_speedHistory.resize(SPEED_HISTORY_SIZE, 0.0);
  m_maxSpeed = 1024.0;
  Refresh();
}

void SpeedGraphPanel::OnPaint(wxPaintEvent &event) {
  wxAutoBufferedPaintDC dc(this);
  wxRect rect = GetClientRect();

  // Fill background
  dc.SetBrush(wxBrush(m_bgColor));
  dc.SetPen(*wxTRANSPARENT_PEN);
  dc.DrawRectangle(rect);

  // Draw components
  DrawGrid(dc, rect);
  DrawSpeedLine(dc, rect);
  DrawLabels(dc, rect);
}

void SpeedGraphPanel::OnSize(wxSizeEvent &event) {
  Refresh();
  event.Skip();
}

void SpeedGraphPanel::DrawGrid(wxDC &dc, const wxRect &rect) {
  dc.SetPen(wxPen(m_gridColor, 1, wxPENSTYLE_DOT));

  // Horizontal grid lines (4 lines)
  int numLines = 4;
  for (int i = 1; i < numLines; ++i) {
    int y = rect.y + (rect.height * i) / numLines;
    dc.DrawLine(rect.x + 50, y, rect.GetRight() - 10, y);
  }

  // Vertical grid lines (every 10 seconds)
  int numVLines = 6;
  for (int i = 1; i < numVLines; ++i) {
    int x = rect.x + 50 + ((rect.width - 60) * i) / numVLines;
    dc.DrawLine(x, rect.y + 10, x, rect.GetBottom() - 20);
  }
}

void SpeedGraphPanel::DrawSpeedLine(wxDC &dc, const wxRect &rect) {
  if (m_speedHistory.empty() || m_maxSpeed <= 0)
    return;

  int graphLeft = rect.x + 50;
  int graphRight = rect.GetRight() - 10;
  int graphTop = rect.y + 10;
  int graphBottom = rect.GetBottom() - 20;
  int graphWidth = graphRight - graphLeft;
  int graphHeight = graphBottom - graphTop;

  if (graphWidth <= 0 || graphHeight <= 0)
    return;

  // Create points for the line
  std::vector<wxPoint> points;
  std::vector<wxPoint> fillPoints;

  // Add bottom-left corner for fill
  fillPoints.push_back(wxPoint(graphLeft, graphBottom));

  size_t numPoints = m_speedHistory.size();
  for (size_t i = 0; i < numPoints; ++i) {
    int x = graphLeft + (graphWidth * i) / (numPoints - 1);
    double normalizedSpeed = m_speedHistory[i] / m_maxSpeed;
    int y = graphBottom - static_cast<int>(normalizedSpeed * graphHeight);
    y = std::max(graphTop, std::min(graphBottom, y));

    points.push_back(wxPoint(x, y));
    fillPoints.push_back(wxPoint(x, y));
  }

  // Add bottom-right corner for fill
  fillPoints.push_back(wxPoint(graphRight, graphBottom));

  // Draw filled area (semi-transparent)
  if (fillPoints.size() >= 3) {
    dc.SetBrush(wxBrush(wxColour(0, 200, 100, 30)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawPolygon(fillPoints.size(), fillPoints.data());
  }

  // Draw the line
  if (points.size() >= 2) {
    dc.SetPen(wxPen(m_lineColor, 2));
    dc.DrawLines(points.size(), points.data());
  }

  // Draw current speed point
  if (!points.empty()) {
    dc.SetBrush(wxBrush(m_lineColor));
    dc.SetPen(wxPen(wxColour(255, 255, 255), 1));
    dc.DrawCircle(points.back(), 4);
  }
}

void SpeedGraphPanel::DrawLabels(wxDC &dc, const wxRect &rect) {
  dc.SetFont(
      wxFont(8, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
  dc.SetTextForeground(m_textColor);

  int graphTop = rect.y + 10;
  int graphBottom = rect.GetBottom() - 20;
  int graphHeight = graphBottom - graphTop;

  // Y-axis labels (speed)
  for (int i = 0; i <= 4; ++i) {
    int y = graphTop + (graphHeight * i) / 4;
    double speed = m_maxSpeed * (4 - i) / 4;
    wxString label = FormatSpeed(speed);
    dc.DrawText(label, rect.x + 2, y - 6);
  }

  // X-axis label (time)
  dc.DrawText("60s", rect.x + 50, rect.GetBottom() - 15);
  dc.DrawText("now", rect.GetRight() - 30, rect.GetBottom() - 15);

  // Title
  dc.SetFont(
      wxFont(9, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
  dc.SetTextForeground(wxColour(200, 200, 200));

  if (!m_speedHistory.empty()) {
    double currentSpeed = m_speedHistory.back();
    wxString title = wxString::Format("Speed: %s", FormatSpeed(currentSpeed));
    dc.DrawText(title, rect.GetRight() - 120, rect.y + 2);
  }
}

wxString SpeedGraphPanel::FormatSpeed(double speedBps) {
  if (speedBps >= 1024.0 * 1024.0 * 1024.0) {
    return wxString::Format("%.1f GB/s", speedBps / (1024.0 * 1024.0 * 1024.0));
  } else if (speedBps >= 1024.0 * 1024.0) {
    return wxString::Format("%.1f MB/s", speedBps / (1024.0 * 1024.0));
  } else if (speedBps >= 1024.0) {
    return wxString::Format("%.1f KB/s", speedBps / 1024.0);
  } else {
    return wxString::Format("%.0f B/s", speedBps);
  }
}

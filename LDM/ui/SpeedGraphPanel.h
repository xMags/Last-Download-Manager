#pragma once
#ifndef SPEED_GRAPH_PANEL_H
#define SPEED_GRAPH_PANEL_H

#include <wx/wx.h>
#include <wx/panel.h>
#include <wx/dcbuffer.h>
#include <deque>
#include <chrono>

class wxGraphicsContext;

// Number of data points to display in the graph
constexpr size_t SPEED_HISTORY_SIZE = 60;

class SpeedGraphPanel : public wxPanel {
public:
    SpeedGraphPanel(wxWindow* parent, wxWindowID id = wxID_ANY);
    virtual ~SpeedGraphPanel() = default;

    // Update the graph with current speed (bytes per second)
    void UpdateSpeed(double speedBps);

    // Clear all speed history
    void Clear();

    // Get the maximum speed recorded
    double GetMaxSpeed() const { return m_maxSpeed; }

private:
    // Event handlers
    void OnPaint(wxPaintEvent& event);
    void OnSize(wxSizeEvent& event);

    // Drawing helpers
    void DrawGrid(wxDC& dc, const wxRect& rect);
    void DrawSpeedLine(wxGraphicsContext* gc, const wxRect& rect);
    void DrawLabels(wxDC& dc, const wxRect& rect);

    // Format speed for display
    static wxString FormatSpeed(double speedBps);

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

    wxDECLARE_EVENT_TABLE();
};

#endif // SPEED_GRAPH_PANEL_H
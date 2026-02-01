#include "SpeedGraphPanel.h"
#include <wx/graphics.h>
#include <wx/dcgraph.h>
#include <algorithm>
#include <cmath>
#include <memory>

wxBEGIN_EVENT_TABLE(SpeedGraphPanel, wxPanel)
    EVT_PAINT(SpeedGraphPanel::OnPaint)
    EVT_SIZE(SpeedGraphPanel::OnSize)
wxEND_EVENT_TABLE()

SpeedGraphPanel::SpeedGraphPanel(wxWindow *parent, wxWindowID id)
    : wxPanel(parent, id, wxDefaultPosition, wxSize(-1, 100)) 
{
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
    double currentMax = 0.0;
    if (!m_speedHistory.empty()) {
        currentMax = *std::max_element(m_speedHistory.begin(), m_speedHistory.end());
    }

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

    // Try creating GC (cast to WindowDC to help resolution if needed, 
    // but usually creating from AutoBufferedPaintDC works fine directly or via MemoryDC)
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc) {
        DrawSpeedLine(gc.get(), rect);
    }

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

    // Vertical grid lines (every 10 seconds approx)
    int numVLines = 6;
    for (int i = 1; i < numVLines; ++i) {
        int x = rect.x + 50 + ((rect.width - 60) * i) / numVLines;
        dc.DrawLine(x, rect.y + 10, x, rect.GetBottom() - 20);
    }
}

void SpeedGraphPanel::DrawSpeedLine(wxGraphicsContext *gc, const wxRect &rect) {
    if (!gc || m_speedHistory.empty() || m_maxSpeed <= 0)
        return;

    int graphLeft = rect.x + 50;
    int graphRight = rect.GetRight() - 10;
    int graphTop = rect.y + 10;
    int graphBottom = rect.GetBottom() - 20;
    int graphWidth = graphRight - graphLeft;
    int graphHeight = graphBottom - graphTop;

    if (graphWidth <= 0 || graphHeight <= 0)
        return;

    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    size_t numPoints = m_speedHistory.size();
    if (numPoints < 2) return;

    // 1. Create path for the filled gradient area
    wxGraphicsPath fillPath = gc->CreatePath();
    fillPath.MoveToPoint(graphLeft, graphBottom); // Start bottom-left

    for (size_t i = 0; i < numPoints; ++i) {
        double x = graphLeft + (double)(graphWidth * i) / (SPEED_HISTORY_SIZE - 1);
        // If history has fewer points than size, we might want to map differently,
        // but typically we want the graph to scroll.
        // The original code used numPoints in denominator, which changes scale as it fills.
        // Let's stick to user's original logic or improve?
        // Original: (double)(graphWidth * i) / (numPoints - 1);
        // This stretches the data to fill width.
        
        x = graphLeft + (double)(graphWidth * i) / (numPoints - 1);

        double normalizedSpeed = m_speedHistory[i] / m_maxSpeed;
        double y = graphBottom - (normalizedSpeed * graphHeight);
        y = std::max((double)graphTop, std::min((double)graphBottom, y));
        fillPath.AddLineToPoint(x, y);
    }

    fillPath.AddLineToPoint(graphRight, graphBottom); // To bottom-right
    fillPath.CloseSubpath();

    // Create vertical gradient brush
    wxGraphicsBrush brush = gc->CreateLinearGradientBrush(
        graphLeft, graphTop, graphLeft, graphBottom, wxColour(0, 200, 100, 150),
        wxColour(0, 200, 100, 5));

    gc->SetBrush(brush);
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->FillPath(fillPath);

    // 2. Draw the speed line on top
    wxGraphicsPath linePath = gc->CreatePath();

    // Calculate first point
    double firstNorm = m_speedHistory[0] / m_maxSpeed;
    double firstY = graphBottom - (firstNorm * graphHeight);
    firstY = std::max((double)graphTop, std::min((double)graphBottom, firstY));

    linePath.MoveToPoint(graphLeft, firstY);

    double lastX = graphLeft;
    double lastY = firstY;

    for (size_t i = 1; i < numPoints; ++i) {
        double x = graphLeft + (double)(graphWidth * i) / (numPoints - 1);
        double normalizedSpeed = m_speedHistory[i] / m_maxSpeed;
        double y = graphBottom - (normalizedSpeed * graphHeight);
        y = std::max((double)graphTop, std::min((double)graphBottom, y));
        linePath.AddLineToPoint(x, y);

        lastX = x;
        lastY = y;
    }

    gc->SetPen(wxPen(m_lineColor, 2));
    gc->StrokePath(linePath);

    // 3. Draw current speed point
    gc->SetBrush(wxBrush(m_lineColor));
    gc->SetPen(wxPen(wxColour(255, 255, 255), 1));
    // DrawCircle takes (x, y, radius) - checking docs, usually DrawEllipse(x,y,w,h)
    gc->DrawEllipse(lastX - 3, lastY - 3, 6, 6);
}

void SpeedGraphPanel::DrawLabels(wxDC &dc, const wxRect &rect) {
    dc.SetFont(wxFont(8, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
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
    dc.SetFont(wxFont(9, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
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
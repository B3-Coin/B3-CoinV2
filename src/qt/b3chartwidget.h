// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3CHARTWIDGET_H
#define BITCOIN_QT_B3CHARTWIDGET_H

#include <QPoint>
#include <QWidget>

class B3CandleSeries;

/**
 * Native OHLCV chart for the trading workspace: candlestick or line
 * mode with volume bars, price/time axes, crosshair with tooltip,
 * wheel zoom, drag pan and autoscale. Purely model-driven from a
 * B3CandleSeries — the widget performs no network access and fabricates
 * no data. Painting clips to the visible index range, so large series
 * cost only the candles on screen.
 */
class B3ChartWidget : public QWidget
{
    Q_OBJECT

public:
    enum class Mode { Candles, Line };

    explicit B3ChartWidget(QWidget* parent = nullptr);

    //! Attach a series (not owned; may be null). Prior connections drop.
    void setSeries(B3CandleSeries* series);
    B3CandleSeries* series() const { return m_series; }

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

    //! Reset the viewport: show the most recent candles, autoscaled.
    void resetView();

    //! Visible half-open candle index range [first, last), clamped to
    //! the series. Exposed for tests.
    QPair<int, int> visibleRange() const;
    //! Number of candles the viewport spans (fractional while zooming).
    double visibleCount() const { return m_visible; }

    //! Programmatic zoom/pan used by toolbar buttons and tests; the
    //! wheel and drag handlers funnel into these.
    void zoomBy(double factor, double anchor_fraction = 0.5);
    void panByCandles(double candles);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void clampView();
    void onSeriesReset();
    void onSeriesUpdated(int index);

    B3CandleSeries* m_series{nullptr};
    Mode m_mode{Mode::Candles};

    // Viewport: first visible candle index (fractional) and span.
    double m_start{0.0};
    double m_visible{60.0};
    bool m_follow_tail{true};

    // Interaction state.
    bool m_dragging{false};
    QPoint m_drag_origin;
    double m_drag_start{0.0};
    QPoint m_hover;
    bool m_hovering{false};
};

#endif // BITCOIN_QT_B3CHARTWIDGET_H

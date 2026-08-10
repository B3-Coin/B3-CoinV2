// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/test/b3tradetests.h>

#include <qt/b3chartwidget.h>
#include <qt/b3fixed.h>
#include <qt/b3marketmodel.h>
#include <qt/b3tradepage.h>

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTest>

#include <memory>

namespace {

//! Deterministic candles for tests only.
QVector<B3Candle> MakeCandles(int count, qint64 base_price = 50'000'000'000LL)
{
    QVector<B3Candle> candles;
    candles.reserve(count);
    for (int i = 0; i < count; ++i) {
        B3Candle candle;
        candle.timestamp = 1'700'000'000LL + i * 3600;
        const qint64 drift = (i % 7 - 3) * 1'000'000LL;
        candle.open = base_price + drift;
        candle.close = base_price + drift + ((i % 2) ? 500'000 : -500'000);
        candle.high = std::max(candle.open, candle.close) + 250'000;
        candle.low = std::min(candle.open, candle.close) - 250'000;
        candle.volume = 1'000'000'000LL + (i % 10) * 50'000'000LL;
        candles.push_back(candle);
    }
    return candles;
}

} // namespace

void B3TradeTests::emptyChartRendersHonestState()
{
    B3ChartWidget chart;
    chart.resize(640, 360);
    QVERIFY(!chart.grab().isNull()); // no series at all
    QCOMPARE(chart.visibleRange().first, chart.visibleRange().second);

    B3CandleSeries series;
    chart.setSeries(&series);
    QVERIFY(!chart.grab().isNull()); // attached but empty
    chart.setSeries(nullptr);
}

void B3TradeTests::oneCandleAndFlatDataRender()
{
    B3CandleSeries series;
    B3ChartWidget chart;
    chart.resize(640, 360);
    chart.setSeries(&series);

    series.setCandles(MakeCandles(1));
    QVERIFY(!chart.grab().isNull());
    const auto [first, last] = chart.visibleRange();
    QCOMPARE(first, 0);
    QCOMPARE(last, 1);

    // Perfectly flat candle (high == low) must not divide by zero.
    B3Candle flat;
    flat.timestamp = 1'800'000'000LL;
    flat.open = flat.high = flat.low = flat.close = 42;
    flat.volume = 0;
    series.setCandles({flat});
    QVERIFY(!chart.grab().isNull());
    chart.setSeries(nullptr);
}

void B3TradeTests::thousandsOfCandlesRenderClipped()
{
    B3CandleSeries series;
    series.setCandles(MakeCandles(5000));
    B3ChartWidget chart;
    chart.resize(800, 400);
    chart.setSeries(&series);

    // Default view shows the tail, not all 5000.
    const auto [first, last] = chart.visibleRange();
    QVERIFY(last == 5000);
    QVERIFY(last - first < 100);
    QVERIFY(!chart.grab().isNull());

    // Line mode over the same data.
    chart.setMode(B3ChartWidget::Mode::Line);
    QVERIFY(!chart.grab().isNull());
    chart.setSeries(nullptr);
}

void B3TradeTests::zoomAndPanRespectBounds()
{
    B3CandleSeries series;
    series.setCandles(MakeCandles(200));
    B3ChartWidget chart;
    chart.resize(800, 400);
    chart.setSeries(&series);

    // Zooming in hard stops at the minimum candle count.
    for (int i = 0; i < 50; ++i) chart.zoomBy(2.0);
    QVERIFY(chart.visibleCount() >= 5.0);

    // Zooming out stops at a bounded multiple of the series length.
    for (int i = 0; i < 50; ++i) chart.zoomBy(0.5);
    QVERIFY(chart.visibleCount() <= 200 * 2.0 + 1.0);

    // Panning cannot push the data fully off screen.
    chart.panByCandles(-1'000'000);
    QVERIFY(chart.visibleRange().second >= 1);
    chart.panByCandles(1'000'000);
    QVERIFY(chart.visibleRange().first <= 199);

    chart.resetView();
    QCOMPARE(chart.visibleRange().second, 200);
    chart.setSeries(nullptr);
}

void B3TradeTests::malformedAndOutOfOrderInputIsSanitized()
{
    B3CandleSeries series;

    B3Candle bad;
    bad.timestamp = 100;
    bad.high = 1;
    bad.low = 10; // high < low → rejected
    bad.open = 5;
    bad.close = 5;

    B3Candle late;
    late.timestamp = 50;
    late.open = late.high = late.low = late.close = 7;

    B3Candle early;
    early.timestamp = 10;
    early.open = early.high = early.low = early.close = 3;

    B3Candle duplicate;
    duplicate.timestamp = 10;
    duplicate.open = duplicate.high = duplicate.low = duplicate.close = 4;

    series.setCandles({bad, late, early, duplicate});
    // bad dropped; duplicate replaced early; sorted ascending.
    QCOMPARE(series.count(), 2);
    QCOMPARE(series.at(0).timestamp, 10);
    QCOMPARE(series.at(0).close, 4);
    QCOMPARE(series.at(1).timestamp, 50);

    // Append with an older timestamp inserts in order; equal replaces.
    B3Candle mid;
    mid.timestamp = 30;
    mid.open = mid.high = mid.low = mid.close = 6;
    series.append(mid);
    QCOMPARE(series.count(), 3);
    QCOMPARE(series.at(1).timestamp, 30);

    mid.close = 8;
    mid.high = 8;
    series.append(mid);
    QCOMPARE(series.count(), 3);
    QCOMPARE(series.at(1).close, 8);
}

void B3TradeTests::incrementalAppendFollowsTail()
{
    B3CandleSeries series;
    series.setCandles(MakeCandles(100));
    B3ChartWidget chart;
    chart.resize(800, 400);
    chart.setSeries(&series);

    QCOMPARE(chart.visibleRange().second, 100);
    B3Candle next = series.at(99);
    next.timestamp += 3600;
    series.append(next);
    // The viewport follows new candles while at the tail.
    QCOMPARE(chart.visibleRange().second, 101);
    QVERIFY(!chart.grab().isNull());
    chart.setSeries(nullptr);
}

void B3TradeTests::orderBookCumulativeAndStates()
{
    B3OrderBookModel bids(B3OrderBookModel::Side::Bid);
    QCOMPARE(bids.rowCount(), 0);
    QCOMPARE(bids.state(), B3OrderBookModel::State::Unavailable);

    bids.setLevels({{100, 5}, {300, 2}, {200, 1}});
    QCOMPARE(bids.rowCount(), 3);
    // Bids sort best (highest) first; totals accumulate downward.
    QCOMPARE(bids.data(bids.index(0, B3OrderBookModel::Price)).toString(), B3Fixed::format(300, 8));
    QCOMPARE(bids.data(bids.index(2, B3OrderBookModel::Total)).toString(), B3Fixed::format(8, 8));
    const double depth = bids.data(bids.index(1, 0), B3OrderBookModel::DepthRole).toDouble();
    QVERIFY(depth > 0.0 && depth <= 1.0);
    QCOMPARE(bids.state(), B3OrderBookModel::State::Live);

    bids.setLevels({});
    QCOMPARE(bids.state(), B3OrderBookModel::State::Empty);

    bids.setState(B3OrderBookModel::State::Unavailable);
    QCOMPARE(bids.rowCount(), 0);

    B3OrderBookModel asks(B3OrderBookModel::Side::Ask);
    asks.setLevels({{300, 1}, {100, 1}});
    QCOMPARE(asks.data(asks.index(0, B3OrderBookModel::Price)).toString(), B3Fixed::format(100, 8));
}

void B3TradeTests::backendUnavailableDisablesSubmission()
{
    B3TradePage page;

    auto* submit = page.findChild<QPushButton*>("ticketSubmit");
    QVERIFY(submit != nullptr);
    QVERIFY(!submit->isEnabled());

    auto* note = page.findChild<QLabel*>("ticketNote");
    QVERIFY(note != nullptr);
    QVERIFY(note->text().contains(QStringLiteral("No trading backend")));

    auto* availability = page.findChild<QLabel*>("tradeAvailability");
    QVERIFY(availability->text().contains(QStringLiteral("unavailable")));

    // No fabricated numbers anywhere in the ticket readouts.
    QCOMPARE(page.findChild<QLabel*>("ticketBalance")->text(), QStringLiteral("—"));
    QCOMPARE(page.findChild<QLabel*>("ticketFee")->text(), QStringLiteral("—"));

    // Orders/positions/fills are model-driven and empty.
    QCOMPARE(page.bids()->rowCount(), 0);
    QCOMPARE(page.asks()->rowCount(), 0);
    QCOMPARE(page.trades()->rowCount(), 0);
}

void B3TradeTests::ticketTotalsAreIntegerExactAcrossPrecisions()
{
    B3TradePage page;
    auto* price = page.findChild<QLineEdit*>("ticketPrice");
    auto* quantity = page.findChild<QLineEdit*>("ticketQuantity");
    auto* total = page.findChild<QLabel*>("ticketTotal");

    // 2 × 3 = 6, computed in raw integer units.
    price->setText(QStringLiteral("2"));
    quantity->setText(QStringLiteral("3"));
    QCOMPARE(total->text(), B3Fixed::format(600'000'000LL, 8));

    // Fractional quantity keeps integer exactness.
    quantity->setText(QStringLiteral("0.00000001"));
    QCOMPARE(total->text(), B3Fixed::format(2LL, 8));

    // Very large input overflows to an explicit state, not a wrap.
    price->setText(QStringLiteral("999999999999"));
    quantity->setText(QStringLiteral("999999999999"));
    QVERIFY(total->text() == QStringLiteral("Too large") || total->text() == QStringLiteral("—"));

    // Cleared input returns to the placeholder.
    price->clear();
    QCOMPARE(total->text(), QStringLiteral("—"));
}

void B3TradeTests::chartSurvivesSeriesDestructionAndReplacement()
{
    B3ChartWidget chart;
    chart.resize(640, 360);

    auto series = std::make_unique<B3CandleSeries>();
    series->setCandles(MakeCandles(50));
    chart.setSeries(series.get());
    QVERIFY(!chart.grab().isNull());

    // Destroying the series must detach the chart, not dangle.
    series.reset();
    QVERIFY(chart.series() == nullptr);
    QVERIFY(!chart.grab().isNull());

    // Replacement with a fresh series works.
    B3CandleSeries replacement;
    replacement.setCandles(MakeCandles(10));
    chart.setSeries(&replacement);
    QCOMPARE(chart.visibleRange().second, 10);
    QVERIFY(!chart.grab().isNull());
    chart.setSeries(nullptr);
}

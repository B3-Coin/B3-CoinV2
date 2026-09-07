// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/b3theme.h>
#include <qt/platformstyle.h>
#include <qt/rpcconsole.h>
#include <qt/forms/ui_debugwindow.h>
#include <util/translation.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOption>
#include <QTest>
#include <QToolButton>

#include <cstdlib>
#include <memory>

const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {
bool NearColor(const QColor& actual, const QColor& expected)
{
    return actual.alpha() > 128 &&
           std::abs(actual.red() - expected.red()) <= 2 &&
           std::abs(actual.green() - expected.green()) <= 2 &&
           std::abs(actual.blue() - expected.blue()) <= 2;
}

int CountColor(const QImage& image, const QColor& color, QRect region = {})
{
    if (region.isEmpty()) region = image.rect();
    region = region.intersected(image.rect());
    int count{0};
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            if (NearColor(image.pixelColor(x, y), color)) ++count;
        }
    }
    return count;
}

QImage Render(QWidget& widget)
{
    widget.ensurePolished();
    QImage image{widget.size(), QImage::Format_ARGB32_Premultiplied};
    image.fill(Qt::transparent);
    widget.render(&image);
    return image;
}

// Deliberately advertises no fixed sizes, like a scalable/native icon engine.
// Different On/Off shapes catch recoloring that loses state-specific art.
class ScalableTestIcon final : public QIconEngine
{
public:
    QIconEngine* clone() const override { return new ScalableTestIcon{*this}; }
    bool isNull() override { return false; }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        // QIconEngine's default pixmap starts uninitialized. This fixture's
        // unpainted area must be transparent before the tinting is tested.
        QPixmap image{size};
        image.fill(Qt::transparent);
        {
            QPainter painter{&image};
            paint(&painter, image.rect(), mode, state);
        }
        return image;
    }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        const int width{rect.width() / 3};
        const int left{state == QIcon::On ? rect.right() - width + 1 : rect.left()};
        // Model the extra dimming applied by some native icon styles. The
        // monochrome engine should not reuse this alpha attenuation when it
        // already supplies a distinct muted disabled color of its own.
        painter->fillRect(QRect{left, rect.top(), width, rect.height()},
                         mode == QIcon::Disabled ? QColor{0, 0, 0, 127} : QColor{Qt::black});
    }
};

class InspectableSpinBox : public QSpinBox
{
public:
    using QSpinBox::QSpinBox;
    QRect part(QStyle::SubControl control) const
    {
        QStyleOptionSpinBox option;
        initStyleOption(&option);
        return style()->subControlRect(QStyle::CC_SpinBox, &option, control, this);
    }
};

class InspectableComboBox : public QComboBox
{
public:
    using QComboBox::QComboBox;
    QRect arrow() const
    {
        QStyleOptionComboBox option;
        initStyleOption(&option);
        return style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
    }
};
} // namespace

class B3ThemeRenderingTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        Q_INIT_RESOURCE(bitcoin);
        B3Theme::apply(*qApp);
    }

    void inheritedIconsAreVisible_data()
    {
        QTest::addColumn<QString>("platform");
        QTest::newRow("macOS") << QStringLiteral("macosx");
        QTest::newRow("Windows") << QStringLiteral("windows");
        QTest::newRow("other") << QStringLiteral("other");
    }

    void inheritedIconsAreVisible()
    {
        QFETCH(QString, platform);
        const std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate(platform)};
        QVERIFY(style != nullptr);
        QVERIFY(style->getImagesOnButtons());
        for (const char* resource : {":/icons/address-book", ":/icons/editpaste", ":/icons/send", ":/icons/remove"}) {
            const QIcon icon{style->SingleColorIcon(QLatin1String(resource))};
            QVERIFY2(!icon.isNull(), resource);
            for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled}) {
                const QPixmap pixmap{icon.pixmap(QSize{24, 24}, 2.0, mode)};
                QVERIFY(!pixmap.isNull());
                QCOMPARE(pixmap.size(), QSize(48, 48));
                QCOMPARE(pixmap.devicePixelRatio(), 2.0);
                const QColor expected{mode == QIcon::Disabled ? B3Theme::kTextMuted : B3Theme::kTextPrimary};
                QVERIFY2(CountColor(pixmap.toImage(), expected) > 10, resource);
            }
        }

        QDialog dialog;
        QToolButton tool{&dialog};
        tool.resize(40, 40);
        tool.setIconSize({24, 24});
        tool.setIcon(style->SingleColorIcon(QStringLiteral(":/icons/editpaste")));
        const QImage normal{Render(tool)};
        QVERIFY(CountColor(normal, B3Theme::kTextPrimary) > 10);
        QVERIFY(CountColor(normal, B3Theme::kCard) > 400);
        tool.setDown(true);
        QVERIFY(CountColor(Render(tool), B3Theme::kAccentMuted) > 400);
        tool.setDown(false);
        tool.setEnabled(false);
        const QImage disabled{Render(tool)};
        QVERIFY(CountColor(disabled, B3Theme::kTextMuted) > 10);
        QVERIFY(CountColor(disabled, B3Theme::kSurface) > 400);

        QPushButton push{&dialog};
        push.resize(100, 40);
        push.setIconSize({24, 24});
        push.setIcon(style->SingleColorIcon(QStringLiteral(":/icons/send")));
        QVERIFY(CountColor(Render(push), B3Theme::kTextPrimary) > 10);
        // Do not turn this into an application-wide tool-button rule: the
        // custom sidebar has its own checked, focused and disabled semantics.
        QVERIFY(!B3Theme::styleSheet().contains(QStringLiteral("\nQToolButton {")));
    }

    void consoleControlsUseDarkSurfaces_data()
    {
        QTest::addColumn<QString>("platform");
        QTest::addColumn<bool>("embedded");
        for (const QString& platform : {QStringLiteral("macosx"), QStringLiteral("windows"), QStringLiteral("other")}) {
            QTest::newRow(qPrintable(platform + QStringLiteral("-window"))) << platform << false;
            QTest::newRow(qPrintable(platform + QStringLiteral("-embedded"))) << platform << true;
        }
    }

    void consoleControlsUseDarkSurfaces()
    {
        QFETCH(QString, platform);
        QFETCH(bool, embedded);
        const std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate(platform)};
        QVERIFY(style != nullptr);

        // Use the production form and its real QWidget ancestry. Putting these
        // controls in a QDialog would hide the missing console stylesheet rule.
        // The form creates no node, wallet, RPC executor or persistent settings.
        QWidget host;
        QWidget console{embedded ? &host : nullptr};
        Ui::RPCConsole ui;
        ui.setupUi(&console);
        QCOMPARE(console.objectName(), QStringLiteral("RPCConsole"));
        QVERIFY(!console.inherits("QDialog"));
        QCOMPARE(console.parentWidget(), embedded ? &host : nullptr);
        ui.tabWidget->setCurrentWidget(ui.tab_console);
        ui.WalletSelector->hide();
        ui.WalletSelectorLabel->hide();
        ui.promptIcon->setIcon(style->SingleColorIcon(QStringLiteral(":/icons/prompticon")));
        console.resize(900, 520);
        QVERIFY(!Render(console).isNull());

        const struct {
            const char* name;
            const char* resource;
        } controls[] = {
            {"fontSmallerButton", ":/icons/fontsmaller"},
            {"fontBiggerButton", ":/icons/fontbigger"},
            {"clearButton", ":/icons/remove"},
        };
        for (const auto& control : controls) {
            auto* button = console.findChild<QToolButton*>(QLatin1String(control.name));
            QVERIFY2(button != nullptr, control.name);
            QCOMPARE(button->iconSize(), QSize(22, 22));
            button->setIcon(style->SingleColorIcon(QLatin1String(control.resource)));
            QVERIFY2(!button->icon().isNull(), control.resource);

            const QImage normal{Render(*button)};
            QVERIFY2(CountColor(normal, B3Theme::kTextPrimary) > 10, control.name);
            QVERIFY2(CountColor(normal, B3Theme::kCard) > 400, control.name);
            button->setDown(true);
            const QImage pressed{Render(*button)};
            QVERIFY2(CountColor(pressed, B3Theme::kTextPrimary) > 10, control.name);
            QVERIFY2(CountColor(pressed, B3Theme::kAccentMuted) > 400, control.name);
            button->setDown(false);
            button->setEnabled(false);
            const QImage disabled{Render(*button)};
            QVERIFY2(CountColor(disabled, B3Theme::kTextMuted) > 10, control.name);
            QVERIFY2(CountColor(disabled, B3Theme::kSurface) > 400, control.name);
            button->setEnabled(true);
        }

        const QString output_dir{qEnvironmentVariable("B3_QT_SCREENSHOT_DIR")};
        if (!output_dir.isEmpty() && platform == QLatin1String("macosx") && !embedded) {
            QVERIFY2(QDir{}.mkpath(output_dir), qPrintable(output_dir));
            ui.messagesWidget->setPlainText(QStringLiteral(
                "B3 Hive RPC console\n\nIsolated visual preview. No node or wallet is connected."));
            QVERIFY(Render(console).save(output_dir + QStringLiteral("/rpc-console.png"), "PNG"));
        }
    }

    void scalableIconsRetainShapesAndScale()
    {
        const std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate(QStringLiteral("macosx"))};
        const QIcon source{new ScalableTestIcon};
        QVERIFY(source.availableSizes().isEmpty());
        const QIcon tinted{style->TextColorIcon(source)};
        QVERIFY(!tinted.isNull());
        for (const auto state : {QIcon::On, QIcon::Off}) {
            const int lit_x{state == QIcon::On ? 42 : 5};
            const int clear_x{state == QIcon::On ? 5 : 42};
            const QImage source_image{source.pixmap(QSize{24, 24}, 2.0, QIcon::Normal, state).toImage()};
            QCOMPARE(source_image.size(), QSize(48, 48));
            QCOMPARE(source_image.pixelColor(clear_x, 24).alpha(), 0);
            const QPixmap image{tinted.pixmap(QSize{24, 24}, 2.0, QIcon::Normal, state)};
            QCOMPARE(image.size(), QSize(48, 48));
            QCOMPARE(image.devicePixelRatio(), 2.0);
            QVERIFY(NearColor(image.toImage().pixelColor(lit_x, 24), B3Theme::kTextPrimary));
            QCOMPARE(image.toImage().pixelColor(clear_x, 24).alpha(), 0);
        }

        QDialog dialog;
        QToolButton button{&dialog};
        button.resize(48, 48);
        button.setIconSize({24, 24});
        button.setIcon(tinted);
        button.setCheckable(true);
        const QImage off{Render(button)};
        button.setChecked(true);
        const QImage on{Render(button)};
        QVERIFY(CountColor(off, B3Theme::kTextPrimary, QRect{10, 10, 12, 28}) > 50);
        QVERIFY(CountColor(on, B3Theme::kTextPrimary, QRect{26, 10, 12, 28}) > 50);
        QVERIFY(off != on);
    }

    void disabledIconsAreTintedOnceWithoutNativeAlphaDimming()
    {
        const std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate(QStringLiteral("windows"))};
        const QIcon source{new ScalableTestIcon};
        const QIcon tinted{style->TextColorIcon(source)};
        for (const auto state : {QIcon::On, QIcon::Off}) {
            const int lit_x{state == QIcon::On ? 42 : 5};
            const int clear_x{state == QIcon::On ? 5 : 42};
            const QImage native{source.pixmap(QSize{24, 24}, 2.0, QIcon::Disabled, state).toImage()};
            QCOMPARE(native.pixelColor(lit_x, 24).alpha(), 127);
            const QPixmap disabled{tinted.pixmap(QSize{24, 24}, 2.0, QIcon::Disabled, state)};
            QCOMPARE(disabled.size(), QSize(48, 48));
            QCOMPARE(disabled.devicePixelRatio(), 2.0);
            const QImage image{disabled.toImage()};
            QCOMPARE(image.pixelColor(lit_x, 24).alpha(), 255);
            QVERIFY(NearColor(image.pixelColor(lit_x, 24), B3Theme::kTextMuted));
            QCOMPARE(image.pixelColor(clear_x, 24).alpha(), 0);
        }
    }

    void checkAndRadioStatesRemainDistinct()
    {
        QDialog dialog;
        QCheckBox check{&dialog};
        check.resize(36, 28);
        check.setTristate(true);
        const QImage unchecked{Render(check)};
        check.setCheckState(Qt::Checked);
        const QImage checked{Render(check)};
        check.setCheckState(Qt::PartiallyChecked);
        const QImage partial{Render(check)};
        QVERIFY(CountColor(unchecked, B3Theme::kAccent) == 0);
        QVERIFY(CountColor(checked, B3Theme::kAccent) > 80);
        QVERIFY(CountColor(partial, B3Theme::kAccent) > 80);
        QVERIFY(checked != partial);
        check.setEnabled(false);
        const QImage disabled_partial{Render(check)};
        QVERIFY(CountColor(disabled_partial, B3Theme::kTextMuted) > 5);
        check.setCheckState(Qt::Checked);
        const QImage disabled_checked{Render(check)};
        QVERIFY(CountColor(disabled_checked, B3Theme::kTextMuted) > 5);
        QVERIFY(disabled_checked != disabled_partial);
        QCOMPARE(check.checkState(), Qt::Checked);

        QRadioButton radio{&dialog};
        radio.resize(36, 28);
        const QImage radio_off{Render(radio)};
        radio.setChecked(true);
        const QImage radio_on{Render(radio)};
        QVERIFY(CountColor(radio_on, B3Theme::kAccent) > 20);
        QVERIFY(radio_off != radio_on);
        radio.setEnabled(false);
        QVERIFY(CountColor(Render(radio), B3Theme::kTextMuted) > 20);
        QVERIFY(radio.isChecked());
    }

    void spinAndComboArrowsRenderAndRespectDisabledState()
    {
        QDialog dialog;
        InspectableSpinBox spin{&dialog};
        spin.resize(140, 38);
        spin.setRange(0, 10);
        spin.setValue(5);
        QImage image{Render(spin)};
        const QRect up{spin.part(QStyle::SC_SpinBoxUp)};
        const QRect down{spin.part(QStyle::SC_SpinBoxDown)};
        QVERIFY(CountColor(image, B3Theme::kTextPrimary, up) > 4);
        QVERIFY(CountColor(image, B3Theme::kTextPrimary, down) > 4);
        QVERIFY(CountColor(image, B3Theme::kCard, up) > 40);
        spin.setValue(10);
        image = Render(spin);
        QVERIFY(CountColor(image, B3Theme::kTextMuted, up) > 4);
        QVERIFY(CountColor(image, B3Theme::kTextPrimary, down) > 4);
        spin.setEnabled(false);
        image = Render(spin);
        QVERIFY(CountColor(image, B3Theme::kTextMuted, up) > 4);
        QVERIFY(CountColor(image, B3Theme::kTextMuted, down) > 4);
        QCOMPARE(spin.value(), 10);

        InspectableComboBox combo{&dialog};
        combo.resize(140, 38);
        combo.addItems({QStringLiteral("First"), QStringLiteral("Second")});
        image = Render(combo);
        const QRect arrow{combo.arrow()};
        QVERIFY(CountColor(image, B3Theme::kTextPrimary, arrow) > 4);
        QVERIFY(CountColor(image, B3Theme::kCard, arrow) > 40);
        combo.setEnabled(false);
        QVERIFY(CountColor(Render(combo), B3Theme::kTextMuted, arrow) > 4);
        QCOMPARE(combo.currentIndex(), 0);
    }
};

QTEST_MAIN(B3ThemeRenderingTests)
#include "b3themerenderingtests.moc"

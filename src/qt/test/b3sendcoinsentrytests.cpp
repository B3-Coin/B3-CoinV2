// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/b3theme.h>
#include <qt/bitcoinaddressvalidator.h>
#include <qt/guiutil.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/sendcoinsentry.h>
#include <util/translation.h>

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QTest>
#include <QToolButton>

#include <memory>

// Match the core test fixture without starting its node/wallet machinery.
const TranslateFn G_TRANSLATION_FUN{nullptr};

class B3SendCoinsEntryTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        Q_INIT_RESOURCE(bitcoin);
        B3Theme::apply(*qApp);
    }

    void actionButtonsStayDarkAndReadable_data()
    {
        QTest::addColumn<QString>("platform");
        QTest::newRow("macOS") << QStringLiteral("macosx");
        QTest::newRow("Windows") << QStringLiteral("windows");
    }

    void actionButtonsStayDarkAndReadable()
    {
        QFETCH(QString, platform);
        std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate(platform)};
        QVERIFY(style != nullptr);
        SendCoinsEntry entry{style.get()};
        entry.ensurePolished();
        for (const char* name : {"addressBookButton", "pasteButton", "deleteButton"}) {
            auto* button = entry.findChild<QToolButton*>(QLatin1String(name));
            QVERIFY2(button != nullptr, name);
            QCOMPARE(button->property("b3variant").toString(), QStringLiteral("recipientAction"));
            QCOMPARE(button->focusPolicy(), Qt::StrongFocus);
            button->ensurePolished();
            button->resize(button->sizeHint());

            // Render the actual scoped stylesheet for each interaction state,
            // without opening a window or using any wallet/clipboard contents.
            const auto render_surface = [&](QStyle::State state) {
                QImage image{button->size(), QImage::Format_ARGB32_Premultiplied};
                image.fill(Qt::transparent);
                QPainter painter{&image};
                QStyleOptionToolButton option;
                option.initFrom(button);
                option.state = state;
                option.subControls = QStyle::SC_ToolButton;
                button->style()->drawComplexControl(QStyle::CC_ToolButton, &option, &painter, button);
                painter.end();
                return image;
            };
            const auto normal = QStyle::State_Enabled | QStyle::State_Raised;
            const QPoint center{button->rect().center()};
            QCOMPARE(render_surface(normal).pixelColor(center), B3Theme::kCard);
            QCOMPARE(render_surface(normal | QStyle::State_MouseOver).pixelColor(center), B3Theme::kCardHover);
            QCOMPARE(render_surface(normal | QStyle::State_Sunken).pixelColor(center), B3Theme::kAccentMuted);
            QCOMPARE(render_surface(QStyle::State_None).pixelColor(center), B3Theme::kSurface);
            const QImage focused{render_surface(normal | QStyle::State_HasFocus)};
            QCOMPARE(focused.pixelColor(focused.width() / 2, 0), B3Theme::kAccent);

            for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled}) {
                const QImage icon{button->icon().pixmap(button->iconSize(), mode).toImage().convertToFormat(QImage::Format_ARGB32)};
                QVERIFY(!icon.isNull());
                QColor strongest;
                for (int y = 0; y < icon.height(); ++y) {
                    for (int x = 0; x < icon.width(); ++x) {
                        const QColor pixel{icon.pixelColor(x, y)};
                        if (pixel.alpha() > strongest.alpha() || !strongest.isValid()) strongest = pixel;
                    }
                }
                QVERIFY(strongest.alpha() > 0);
                const QColor expected{mode == QIcon::Disabled ? B3Theme::kTextMuted : B3Theme::kTextPrimary};
                QCOMPARE(strongest.rgb(), expected.rgb());
            }
        }
        // The selector must not accidentally become a global tool-button rule.
        QVERIFY(!B3Theme::styleSheet().contains(QStringLiteral("\nQToolButton {")));
    }

    void addressPlaceholderDoesNotSuggestAnotherChainOrRecipient()
    {
        QWidget parent;
        QValidatedLineEdit address{&parent};
        GUIUtil::setupAddressWidget(&address, &parent);
        QCOMPARE(address.placeholderText(), QObject::tr("Enter a B3 address"));
        QVERIFY(address.text().isEmpty());
        QVERIFY(qobject_cast<const BitcoinAddressEntryValidator*>(address.validator()) != nullptr);
        QVERIFY(parent.findChild<BitcoinAddressCheckValidator*>() != nullptr);
        QCOMPARE(parent.focusProxy(), &address);
    }
};

QTEST_MAIN(B3SendCoinsEntryTests)
#include "b3sendcoinsentrytests.moc"

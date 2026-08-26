// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3navsidebar.h>

#include <qt/b3theme.h>

#include <QAbstractButton>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QSize>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

namespace {

QPixmap sidebarGlyphPixmap(B3Page page, int logicalSize, qreal devicePixelRatio,
                           const QColor& color)
{
    const int physicalSize = qRound(logicalSize * devicePixelRatio);
    QPixmap glyph{physicalSize, physicalSize};
    glyph.fill(Qt::transparent);
    QPainter painter{&glyph};
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(devicePixelRatio, devicePixelRatio);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});

    switch (page) {
    case B3Page::Dashboard:
        painter.drawPolyline(QPolygonF{{2.5, 8.0}, {8.0, 2.8}, {13.5, 8.0}});
        painter.drawPath([] {
            QPainterPath path;
            path.moveTo(4.0, 7.0);
            path.lineTo(4.0, 13.0);
            path.lineTo(12.0, 13.0);
            path.lineTo(12.0, 7.0);
            path.moveTo(6.8, 13.0);
            path.lineTo(6.8, 9.3);
            path.lineTo(9.2, 9.3);
            path.lineTo(9.2, 13.0);
            return path;
        }());
        break;
    case B3Page::Trade:
        painter.drawPolyline(QPolygonF{{2.5, 12.5}, {6.3, 8.2}, {9.2, 9.8}, {13.3, 3.5}});
        painter.drawPolyline(QPolygonF{{10.1, 3.5}, {13.3, 3.5}, {13.3, 6.7}});
        break;
    case B3Page::Assets:
        painter.drawRoundedRect(QRectF{2.5, 5.0, 11.0, 8.0}, 1.0, 1.0);
        painter.drawLine(QPointF{2.8, 7.0}, QPointF{13.2, 7.0});
        painter.drawEllipse(QPointF{10.7, 9.8}, 0.8, 0.8);
        painter.drawPolyline(QPolygonF{{5.0, 5.0}, {5.0, 3.2}, {11.0, 3.2}, {11.0, 5.0}});
        break;
    case B3Page::Stake:
        painter.drawEllipse(QPointF{8.0, 3.4}, 1.5, 1.5);
        painter.drawLine(QPointF{8.0, 5.0}, QPointF{8.0, 13.5});
        painter.drawArc(QRectF{3.0, 5.3, 10.0, 9.0}, 18 * 16, 144 * 16);
        painter.drawLine(QPointF{5.0, 13.0}, QPointF{8.0, 9.0});
        painter.drawLine(QPointF{11.0, 13.0}, QPointF{8.0, 9.0});
        break;
    case B3Page::Activity:
        painter.drawLine(QPointF{2.5, 4.0}, QPointF{13.5, 4.0});
        painter.drawLine(QPointF{2.5, 8.0}, QPointF{13.5, 8.0});
        painter.drawLine(QPointF{2.5, 12.0}, QPointF{13.5, 12.0});
        painter.drawEllipse(QPointF{6.0, 4.0}, 1.3, 1.3);
        painter.drawEllipse(QPointF{10.5, 8.0}, 1.3, 1.3);
        painter.drawEllipse(QPointF{5.0, 12.0}, 1.3, 1.3);
        break;
    case B3Page::Settings:
        painter.drawEllipse(QPointF{8.0, 8.0}, 2.5, 2.5);
        for (int spoke = 0; spoke < 8; ++spoke) {
            const qreal angle{spoke * 3.14159265358979323846 / 4.0};
            painter.drawLine(QPointF{8.0 + std::cos(angle) * 4.2, 8.0 + std::sin(angle) * 4.2},
                             QPointF{8.0 + std::cos(angle) * 6.2, 8.0 + std::sin(angle) * 6.2});
        }
        break;
    }
    painter.end();
    glyph.setDevicePixelRatio(devicePixelRatio);
    return glyph;
}

QIcon sidebarIcon(B3Page page)
{
    QIcon icon;
    for (int scale = 1; scale <= 3; ++scale) {
        const qreal dpr = scale;
        icon.addPixmap(sidebarGlyphPixmap(page, B3Theme::kIconSm, dpr, B3Theme::kTextSecondary),
                       QIcon::Normal, QIcon::Off);
        icon.addPixmap(sidebarGlyphPixmap(page, B3Theme::kIconSm, dpr, B3Theme::kTextPrimary),
                       QIcon::Active, QIcon::Off);
        icon.addPixmap(sidebarGlyphPixmap(page, B3Theme::kIconSm, dpr, B3Theme::kAccent),
                       QIcon::Normal, QIcon::On);
        icon.addPixmap(sidebarGlyphPixmap(page, B3Theme::kIconSm, dpr, B3Theme::kAccent),
                       QIcon::Active, QIcon::On);
        icon.addPixmap(sidebarGlyphPixmap(page, B3Theme::kIconSm, dpr, B3Theme::kTextMuted),
                       QIcon::Disabled, QIcon::Off);
    }
    return icon;
}

QPixmap hiveMarkPixmap(int logicalSize, qreal devicePixelRatio)
{
    const int physicalSize = qRound(logicalSize * devicePixelRatio);
    QPixmap mark{physicalSize, physicalSize};
    mark.fill(Qt::transparent);
    QPainter painter{&mark};
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(devicePixelRatio, devicePixelRatio);

    // The compact product mark is drawn natively so it stays sharp at every
    // device scale and exactly matches the approved outlined-hex direction.
    const qreal s{static_cast<qreal>(logicalSize)};
    QPolygonF hex;
    hex << QPointF{s * 0.28, s * 0.06} << QPointF{s * 0.72, s * 0.06}
        << QPointF{s * 0.94, s * 0.50} << QPointF{s * 0.72, s * 0.94}
        << QPointF{s * 0.28, s * 0.94} << QPointF{s * 0.06, s * 0.50};
    painter.setBrush(QColor{0x18, 0x16, 0x0f});
    painter.setPen(QPen{B3Theme::kAccent, 1.6});
    painter.drawPolygon(hex);

    QFont b_font{painter.font()};
    b_font.setBold(true);
    b_font.setPixelSize(qRound(s * 0.42));
    painter.setFont(b_font);
    painter.setPen(B3Theme::kAccent);
    painter.drawText(QRectF{s * 0.20, s * 0.18, s * 0.56, s * 0.64},
                     Qt::AlignCenter, QStringLiteral("B"));

    QFont three_font{b_font};
    three_font.setPixelSize(qRound(s * 0.18));
    painter.setFont(three_font);
    painter.setPen(QColor{0xf6, 0xdc, 0x9a});
    painter.drawText(QRectF{s * 0.61, s * 0.23, s * 0.18, s * 0.25},
                     Qt::AlignCenter, QStringLiteral("3"));
    painter.end();
    mark.setDevicePixelRatio(devicePixelRatio);
    return mark;
}

} // namespace

B3NavSidebar::B3NavSidebar(QWidget* parent)
    : QFrame{parent}
{
    setObjectName("B3Sidebar");
    setFixedWidth(B3Theme::kSidebarWidth);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(B3Theme::kSpaceMd, B3Theme::kSpaceLg, B3Theme::kSpaceMd,
                                 B3Theme::kSpaceMd);
    m_layout->setSpacing(B3Theme::kSpaceXs);

    auto* brandRow = new QWidget(this);
    brandRow->setObjectName("B3SidebarBrandRow");
    auto* brandRowLayout = new QHBoxLayout(brandRow);
    brandRowLayout->setContentsMargins(0, 0, 0, 0);
    brandRowLayout->setSpacing(B3Theme::kSpaceSm);

    m_mark = new QLabel(brandRow);
    m_mark->setObjectName("B3SidebarMark");
    m_mark->setPixmap(hiveMarkPixmap(40, devicePixelRatioF()));
    m_mark->setAlignment(Qt::AlignCenter);
    m_mark->setFixedSize(42, 42);
    m_mark->setAccessibleName(tr("B3 Hive mark"));
    brandRowLayout->addWidget(m_mark);

    m_brandCopy = new QWidget(brandRow);
    auto* brandCopyLayout = new QVBoxLayout(m_brandCopy);
    brandCopyLayout->setContentsMargins(0, 0, 0, 0);
    brandCopyLayout->setSpacing(1);
    auto* brand = new QLabel(tr("B3 HIVE"), m_brandCopy);
    brand->setObjectName("B3SidebarBrand");
    auto* brandSub = new QLabel(tr("DESKTOP"), m_brandCopy);
    brandSub->setObjectName("B3SidebarBrandSub");
    brandCopyLayout->addWidget(brand);
    brandCopyLayout->addWidget(brandSub);
    brandRowLayout->addWidget(m_brandCopy, 1);
    m_layout->addWidget(brandRow);
    m_layout->addSpacing(B3Theme::kSpaceLg);

    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);

    addItem(B3Page::Dashboard, tr("Overview"), "navDashboard");
    addItem(B3Page::Trade, tr("Trade"), "navTrade");
    addItem(B3Page::Assets, tr("Assets"), "navAssets");
    addItem(B3Page::Stake, tr("Stake"), "navStake");
    addItem(B3Page::Activity, tr("Activity"), "navActivity");
    m_layout->addStretch(1);
    addItem(B3Page::Settings, tr("Settings"), "navSettings");

    m_platform = new QLabel(tr("B3 HIVE DESKTOP\nMODERN FEATURES INACTIVE"), this);
    m_platform->setObjectName("B3SidebarPlatform");
    m_platform->setWordWrap(true);
    m_platform->setAccessibleName(tr("B3 Hive desktop; modern features inactive"));
    m_layout->addSpacing(B3Theme::kSpaceMd);
    m_layout->addWidget(m_platform);

    if (auto* first = qobject_cast<QToolButton*>(m_group->button(0))) {
        first->setChecked(true);
    }

    connect(m_group, &QButtonGroup::idClicked, this, [this](int id) {
        m_current = static_cast<B3Page>(id);
        Q_EMIT navigated(m_current);
    });
}

QToolButton* B3NavSidebar::addItem(B3Page page, const QString& text, const QString& objectName)
{
    auto* button = new QToolButton(this);
    button->setObjectName(objectName);
    button->setText(text);
    button->setIcon(sidebarIcon(page));
    button->setIconSize(QSize{B3Theme::kIconSm, B3Theme::kIconSm});
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setAccessibleName(text);
    button->setAccessibleDescription(tr("Navigate to the %1 page").arg(text));
    m_group->addButton(button, static_cast<int>(page));
    m_layout->addWidget(button);
    return button;
}

void B3NavSidebar::setCompact(bool compact)
{
    if (m_compact == compact) return;
    m_compact = compact;
    setFixedWidth(compact ? B3Theme::kSidebarCompactWidth : B3Theme::kSidebarWidth);
    m_layout->setContentsMargins(compact ? 15 : B3Theme::kSpaceMd,
                                 B3Theme::kSpaceLg,
                                 compact ? 15 : B3Theme::kSpaceMd,
                                 B3Theme::kSpaceMd);
    m_brandCopy->setVisible(!compact);
    m_platform->setVisible(!compact);
    m_mark->setFixedSize(compact ? 40 : 42, compact ? 40 : 42);
    m_mark->setPixmap(hiveMarkPixmap(compact ? 36 : 40, devicePixelRatioF()));
    for (QAbstractButton* abstractButton : m_group->buttons()) {
        auto* button = qobject_cast<QToolButton*>(abstractButton);
        if (!button) continue;
        if (compact) {
            button->setFixedWidth(40);
        } else {
            button->setMinimumWidth(0);
            button->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        button->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
        button->setToolTip(compact ? button->text() : QString{});
    }
}

void B3NavSidebar::setCurrentPage(B3Page page)
{
    m_current = page;
    if (auto* button = m_group->button(static_cast<int>(page))) {
        const QSignalBlocker blocker{m_group};
        button->setChecked(true);
    }
}

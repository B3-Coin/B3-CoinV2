// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3navsidebar.h>

#include <qt/b3theme.h>

#include <QButtonGroup>
#include <QLabel>
#include <QPixmap>
#include <QToolButton>
#include <QVBoxLayout>

B3NavSidebar::B3NavSidebar(QWidget* parent)
    : QFrame{parent}
{
    setObjectName("B3Sidebar");
    setFixedWidth(B3Theme::kSidebarWidth);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(B3Theme::kSpaceMd, B3Theme::kSpaceLg, B3Theme::kSpaceMd,
                                 B3Theme::kSpaceMd);
    m_layout->setSpacing(B3Theme::kSpaceXs);

    auto* mark = new QLabel(this);
    mark->setObjectName("B3SidebarMark");
    mark->setPixmap(QPixmap(QStringLiteral(":/icons/b3hive_mark"))
                        .scaled(56, 56, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    mark->setAlignment(Qt::AlignHCenter);
    m_layout->addWidget(mark);
    auto* brand = new QLabel(tr("B3FlowMesh"), this);
    brand->setObjectName("B3SidebarBrand");
    auto* brandSub = new QLabel(tr("Wallet"), this);
    brandSub->setObjectName("B3SidebarBrandSub");
    m_layout->addWidget(brand);
    m_layout->addWidget(brandSub);
    m_layout->addSpacing(B3Theme::kSpaceLg);

    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);

    addItem(B3Page::Dashboard, tr("Dashboard"), "navDashboard");
    addItem(B3Page::Trade, tr("Trade"), "navTrade");
    addItem(B3Page::Assets, tr("Assets"), "navAssets");
    addItem(B3Page::Stake, tr("Stake"), "navStake");
    addItem(B3Page::Activity, tr("Activity"), "navActivity");
    m_layout->addStretch(1);
    addItem(B3Page::Settings, tr("Settings"), "navSettings");

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
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setAccessibleName(text);
    button->setAccessibleDescription(tr("Navigate to the %1 page").arg(text));
    m_group->addButton(button, static_cast<int>(page));
    m_layout->addWidget(button);
    return button;
}

void B3NavSidebar::setCurrentPage(B3Page page)
{
    m_current = page;
    if (auto* button = m_group->button(static_cast<int>(page))) {
        const QSignalBlocker blocker{m_group};
        button->setChecked(true);
    }
}

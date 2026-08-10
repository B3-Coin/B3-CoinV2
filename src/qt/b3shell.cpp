// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3shell.h>

#include <qt/b3navsidebar.h>
#include <qt/b3placeholderpage.h>
#include <qt/b3theme.h>
#include <qt/b3topstatus.h>

#include <QHBoxLayout>
#include <QStackedWidget>
#include <QVBoxLayout>

B3Shell::B3Shell(QWidget* parent)
    : QWidget{parent}
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_topStatus = new B3TopStatus(this);
    outer->addWidget(m_topStatus);

    auto* body = new QWidget(this);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    m_sidebar = new B3NavSidebar(body);
    bodyLayout->addWidget(m_sidebar);

    m_content = new QStackedWidget(body);
    bodyLayout->addWidget(m_content, 1);
    outer->addWidget(body, 1);

    // Wallet host (filled by setWalletWidget), plus honest placeholders.
    m_walletHost = new QWidget(m_content);
    auto* hostLayout = new QVBoxLayout(m_walletHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    m_walletIndex = m_content->addWidget(m_walletHost);

    m_tradeIndex = m_content->addWidget(new B3PlaceholderPage(
        tr("Trade"),
        tr("The B3FlowMesh trading workspace connects here. No trading "
           "backend is available in this build, so no markets, orders or "
           "prices are shown."),
        m_content));
    m_assetsIndex = m_content->addWidget(new B3PlaceholderPage(
        tr("Assets"),
        tr("Coloured assets and FlowMesh balances will appear here. Only "
           "native B3 is available in this build."),
        m_content));
    m_stakeIndex = m_content->addWidget(new B3PlaceholderPage(
        tr("Stake"),
        tr("Staking information will appear here when the wallet exposes a "
           "staking model."),
        m_content));

    connect(m_sidebar, &B3NavSidebar::navigated, this, [this](B3Page page) {
        showPage(page);
        Q_EMIT pageSelected(page);
    });

    m_content->setCurrentIndex(m_walletIndex);
}

void B3Shell::setWalletWidget(QWidget* wallet_widget)
{
    if (!wallet_widget) return;
    auto* layout = qobject_cast<QVBoxLayout*>(m_walletHost->layout());
    // Remove any prior content without deleting externally-owned widgets.
    while (layout->count() > 0) {
        QLayoutItem* item = layout->takeAt(0);
        if (item->widget()) item->widget()->setParent(nullptr);
        delete item;
    }
    wallet_widget->setParent(m_walletHost);
    layout->addWidget(wallet_widget);
}

void B3Shell::showPage(B3Page page)
{
    m_sidebar->setCurrentPage(page);
    switch (page) {
    case B3Page::Dashboard:
    case B3Page::Activity:
        m_content->setCurrentIndex(m_walletIndex);
        break;
    case B3Page::Trade:
        m_content->setCurrentIndex(m_tradeIndex);
        break;
    case B3Page::Assets:
        m_content->setCurrentIndex(m_assetsIndex);
        break;
    case B3Page::Stake:
        m_content->setCurrentIndex(m_stakeIndex);
        break;
    case B3Page::Settings:
        // With an installed Settings page, show it; otherwise the window
        // opens the existing options dialog and the stack stays put.
        if (m_settingsIndex >= 0) m_content->setCurrentIndex(m_settingsIndex);
        break;
    }
}

void B3Shell::replacePage(int index, QWidget* page)
{
    if (!page || index < 0) return;
    QWidget* old = m_content->widget(index);
    page->setParent(m_content);
    m_content->insertWidget(index, page);
    if (old) {
        m_content->removeWidget(old);
        old->deleteLater();
    }
}

void B3Shell::setTradePage(QWidget* page) { replacePage(m_tradeIndex, page); }
void B3Shell::setAssetsPage(QWidget* page) { replacePage(m_assetsIndex, page); }
void B3Shell::setStakePage(QWidget* page) { replacePage(m_stakeIndex, page); }

void B3Shell::setSettingsPage(QWidget* page)
{
    if (!page) return;
    if (m_settingsIndex >= 0) {
        replacePage(m_settingsIndex, page);
    } else {
        page->setParent(m_content);
        m_settingsIndex = m_content->addWidget(page);
    }
}

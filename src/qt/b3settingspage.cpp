// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3settingspage.h>

#include <qt/b3theme.h>

#include <QAction>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {
QWidget* MakeCard(QWidget* parent, const QString& title, const QString& body, QVBoxLayout** layout_out)
{
    auto* card = new QFrame(parent);
    B3Theme::markCard(card);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(B3Theme::kSpaceLg, B3Theme::kSpaceLg,
                               B3Theme::kSpaceLg, B3Theme::kSpaceLg);
    layout->setSpacing(B3Theme::kSpaceSm);
    auto* title_label = new QLabel(title, card);
    B3Theme::markTextRole(title_label, QStringLiteral("h3"));
    layout->addWidget(title_label);
    auto* body_label = new QLabel(body, card);
    body_label->setWordWrap(true);
    B3Theme::markTextRole(body_label, QStringLiteral("secondary"));
    layout->addWidget(body_label);
    *layout_out = layout;
    return card;
}
} // namespace

B3SettingsPage::B3SettingsPage(QWidget* parent)
    : QWidget{parent}
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    m_content = new QWidget(scroll);
    m_content->setObjectName(QStringLiteral("settingsViewport"));
    auto* layout = new QVBoxLayout(m_content);
    layout->setContentsMargins(B3Theme::kSpaceLg, B3Theme::kSpaceLg,
                               B3Theme::kSpaceLg, B3Theme::kSpaceXl);
    layout->setSpacing(B3Theme::kSpaceLg);

    auto* eyebrow = new QLabel(tr("B3 HIVE"), m_content);
    B3Theme::markTextRole(eyebrow, QStringLiteral("eyebrow"));
    layout->addWidget(eyebrow);
    auto* heading = new QLabel(tr("Settings"), m_content);
    B3Theme::markTextRole(heading, QStringLiteral("h1"));
    layout->addWidget(heading);
    auto* introduction = new QLabel(
        tr("Configure the application, network and wallet using the established B3 dialogs and security actions."),
        m_content);
    introduction->setWordWrap(true);
    B3Theme::markTextRole(introduction, QStringLiteral("secondary"));
    layout->addWidget(introduction);

    m_card_grid = new QGridLayout();
    m_card_grid->setContentsMargins(0, 0, 0, 0);
    m_card_grid->setHorizontalSpacing(B3Theme::kSpaceMd);
    m_card_grid->setVerticalSpacing(B3Theme::kSpaceMd);

    QVBoxLayout* generalLayout{nullptr};
    m_application_card = MakeCard(m_content, tr("Application"),
        tr("Startup behavior, wallet options, display units and language. "
           "All settings keep their existing dialogs and validation, "
           "including Reset options."),
        &generalLayout);
    m_application_card->setObjectName(QStringLiteral("settingsApplicationCard"));
    m_application_card->setProperty("b3surface", QStringLiteral("panel"));
    {
        auto* open = new QPushButton(tr("Open settings"), m_application_card);
        open->setObjectName(QStringLiteral("settingsOpenMain"));
        open->setProperty("b3variant", QStringLiteral("primary"));
        connect(open, &QPushButton::clicked, this,
                [this] { Q_EMIT openOptionsRequested(OptionsDialog::TAB_MAIN); });
        generalLayout->addWidget(open, 0, Qt::AlignLeft);
        generalLayout->addStretch();
    }

    QVBoxLayout* networkLayout{nullptr};
    m_network_card = MakeCard(m_content, tr("Network"),
        tr("Connectivity, port mapping and proxy configuration, unchanged "
           "from the existing network options."),
        &networkLayout);
    m_network_card->setObjectName(QStringLiteral("settingsNetworkCard"));
    m_network_card->setProperty("b3surface", QStringLiteral("panel"));
    {
        auto* open = new QPushButton(tr("Open network settings"), m_network_card);
        open->setObjectName(QStringLiteral("settingsOpenNetwork"));
        connect(open, &QPushButton::clicked, this,
                [this] { Q_EMIT openOptionsRequested(OptionsDialog::TAB_NETWORK); });
        networkLayout->addWidget(open, 0, Qt::AlignLeft);
        networkLayout->addStretch();
    }

    QVBoxLayout* walletLayout{nullptr};
    m_wallet_card = MakeCard(m_content, tr("Wallet security"),
        tr("Encryption, passphrase and backup use the existing wallet "
           "dialogs; nothing sensitive is stored by this page."),
        &walletLayout);
    m_wallet_card->setObjectName(QStringLiteral("settingsWalletCard"));
    m_wallet_card->setProperty("b3surface", QStringLiteral("quiet"));
    {
        m_wallet_actions_layout = walletLayout;
        m_wallet_note = new QLabel(tr("No wallet actions are available."), m_wallet_card);
        B3Theme::markTextRole(m_wallet_note, QStringLiteral("status"));
        walletLayout->addWidget(m_wallet_note);
        walletLayout->addStretch();
    }

    layout->addLayout(m_card_grid);
    m_layout = layout;
    layout->addStretch();
    scroll->setWidget(m_content);
    reflowCards(width());
}

void B3SettingsPage::setUpdateWidget(QWidget* widget)
{
    if (!widget || !m_layout) return;
    widget->setParent(m_content);
    // Insert above the trailing stretch.
    m_layout->insertWidget(m_layout->count() - 1, widget);
}

void B3SettingsPage::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    reflowCards(event->size().width());
}

void B3SettingsPage::reflowCards(int width)
{
    if (!m_card_grid || !m_wallet_card) return;
    const int columns = width < 720 ? 1 : (width < 1060 ? 2 : 3);
    if (columns == m_layout_columns) return;
    m_layout_columns = columns;

    for (QWidget* card : {m_application_card, m_network_card, m_wallet_card}) {
        m_card_grid->removeWidget(card);
    }
    for (int column = 0; column < 3; ++column) {
        m_card_grid->setColumnStretch(column, column < columns ? 1 : 0);
    }
    if (columns == 1) {
        m_card_grid->addWidget(m_application_card, 0, 0);
        m_card_grid->addWidget(m_network_card, 1, 0);
        m_card_grid->addWidget(m_wallet_card, 2, 0);
    } else if (columns == 2) {
        m_card_grid->addWidget(m_application_card, 0, 0);
        m_card_grid->addWidget(m_network_card, 0, 1);
        m_card_grid->addWidget(m_wallet_card, 1, 0, 1, 2);
    } else {
        m_card_grid->addWidget(m_application_card, 0, 0);
        m_card_grid->addWidget(m_network_card, 0, 1);
        m_card_grid->addWidget(m_wallet_card, 0, 2);
    }
}

void B3SettingsPage::setWalletActions(const QList<QAction*>& actions)
{
    bool any{false};
    for (QAction* action : actions) {
        if (!action) continue;
        any = true;
        auto* button = new QPushButton(action->text(), m_wallet_card);
        button->setEnabled(action->isEnabled());
        connect(button, &QPushButton::clicked, action, &QAction::trigger);
        connect(action, &QAction::changed, button, [action, button] {
            button->setEnabled(action->isEnabled());
            button->setText(action->text());
        });
        // Insert above the trailing stretch.
        m_wallet_actions_layout->insertWidget(m_wallet_actions_layout->count() - 1, button, 0, Qt::AlignLeft);
    }
    m_wallet_note->setVisible(!any);
}

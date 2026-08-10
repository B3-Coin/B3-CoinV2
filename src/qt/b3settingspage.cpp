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
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
QWidget* MakeCard(QWidget* parent, const QString& title, const QString& body, QVBoxLayout** layout_out)
{
    auto* card = new QFrame(parent);
    B3Theme::markCard(card);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(B3Theme::kSpaceMd, B3Theme::kSpaceMd, B3Theme::kSpaceMd, B3Theme::kSpaceMd);
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
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(B3Theme::kSpaceLg, B3Theme::kSpaceLg, B3Theme::kSpaceLg, B3Theme::kSpaceLg);
    layout->setSpacing(B3Theme::kSpaceMd);

    auto* heading = new QLabel(tr("Settings"), this);
    B3Theme::markTextRole(heading, QStringLiteral("h1"));
    layout->addWidget(heading);

    auto* row = new QHBoxLayout();
    row->setSpacing(B3Theme::kSpaceMd);

    QVBoxLayout* generalLayout{nullptr};
    QWidget* generalCard = MakeCard(this, tr("Application"),
        tr("Startup behavior, wallet options, display units and language. "
           "All settings keep their existing dialogs and validation, "
           "including Reset options."),
        &generalLayout);
    {
        auto* open = new QPushButton(tr("Open settings"), generalCard);
        open->setObjectName(QStringLiteral("settingsOpenMain"));
        connect(open, &QPushButton::clicked, this,
                [this] { Q_EMIT openOptionsRequested(OptionsDialog::TAB_MAIN); });
        generalLayout->addWidget(open, 0, Qt::AlignLeft);
        generalLayout->addStretch();
    }
    row->addWidget(generalCard, 1);

    QVBoxLayout* networkLayout{nullptr};
    QWidget* networkCard = MakeCard(this, tr("Network"),
        tr("Connectivity, port mapping and proxy configuration, unchanged "
           "from the existing network options."),
        &networkLayout);
    {
        auto* open = new QPushButton(tr("Open network settings"), networkCard);
        open->setObjectName(QStringLiteral("settingsOpenNetwork"));
        connect(open, &QPushButton::clicked, this,
                [this] { Q_EMIT openOptionsRequested(OptionsDialog::TAB_NETWORK); });
        networkLayout->addWidget(open, 0, Qt::AlignLeft);
        networkLayout->addStretch();
    }
    row->addWidget(networkCard, 1);

    QVBoxLayout* walletLayout{nullptr};
    QWidget* walletCard = MakeCard(this, tr("Wallet security"),
        tr("Encryption, passphrase and backup use the existing wallet "
           "dialogs; nothing sensitive is stored by this page."),
        &walletLayout);
    {
        m_wallet_actions_layout = walletLayout;
        m_wallet_note = new QLabel(tr("No wallet actions are available."), walletCard);
        B3Theme::markTextRole(m_wallet_note, QStringLiteral("secondary"));
        walletLayout->addWidget(m_wallet_note);
        walletLayout->addStretch();
    }
    row->addWidget(walletCard, 1);

    layout->addLayout(row);
    layout->addStretch();
}

void B3SettingsPage::setWalletActions(const QList<QAction*>& actions)
{
    bool any{false};
    for (QAction* action : actions) {
        if (!action) continue;
        any = true;
        auto* button = new QPushButton(action->text(), this);
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

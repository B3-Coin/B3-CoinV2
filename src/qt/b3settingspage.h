// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3SETTINGSPAGE_H
#define BITCOIN_QT_B3SETTINGSPAGE_H

#include <qt/optionsdialog.h>

#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QVBoxLayout;
QT_END_NAMESPACE

/**
 * The Settings page: organizes access to the existing configuration
 * surfaces inside the shell without changing what any setting means.
 * Options continue to live in the existing OptionsDialog (validation,
 * reset and apply behavior untouched); wallet security operations
 * continue to be the existing QActions. Nothing is stored here.
 */
class B3SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit B3SettingsPage(QWidget* parent = nullptr);

    //! Offer existing wallet actions (encrypt, backup, change
    //! passphrase…) as buttons. The actions stay owned by the window;
    //! button enabled-state mirrors the action's.
    void setWalletActions(const QList<QAction*>& actions);

Q_SIGNALS:
    //! Ask the window to open the existing options dialog at a tab.
    void openOptionsRequested(OptionsDialog::Tab tab);

private:
    QVBoxLayout* m_wallet_actions_layout{nullptr};
    QLabel* m_wallet_note{nullptr};
};

#endif // BITCOIN_QT_B3SETTINGSPAGE_H

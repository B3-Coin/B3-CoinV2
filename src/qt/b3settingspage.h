// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3SETTINGSPAGE_H
#define BITCOIN_QT_B3SETTINGSPAGE_H

#include <qt/optionsdialog.h>

#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QGridLayout;
class QResizeEvent;
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

    //! Install the "B3 Hive update" section (owned by the caller's tree).
    void setUpdateWidget(QWidget* widget);

Q_SIGNALS:
    //! Ask the window to open the existing options dialog at a tab.
    void openOptionsRequested(OptionsDialog::Tab tab);

private:
    void resizeEvent(QResizeEvent* event) override;
    void reflowCards(int width);

    QVBoxLayout* m_layout{nullptr};
    QWidget* m_content{nullptr};
    QVBoxLayout* m_wallet_actions_layout{nullptr};
    QLabel* m_wallet_note{nullptr};
    QGridLayout* m_card_grid{nullptr};
    QWidget* m_application_card{nullptr};
    QWidget* m_network_card{nullptr};
    QWidget* m_wallet_card{nullptr};
    int m_layout_columns{0};
};

#endif // BITCOIN_QT_B3SETTINGSPAGE_H

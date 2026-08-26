// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_QT_UPDATEPANEL_H
#define B3COIN_QT_UPDATEPANEL_H

#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QPushButton;
QT_END_NAMESPACE

class UpdateController;

/** The "B3 Hive update" section for the Settings page: installed version,
 *  latest verified version, release-notes digest, download size and the
 *  operations actually supported by this build. Wording rule: always a
 *  "B3 Hive update", never a "new wallet". Unconfigured builds show an
 *  honest disabled note and never touch the network. */
class UpdatePanel : public QWidget
{
    Q_OBJECT

public:
    explicit UpdatePanel(UpdateController* controller, QWidget* parent = nullptr);

private Q_SLOTS:
    void refresh();
    void onInstallClicked();

private:
    UpdateController* m_controller;
    QLabel* m_installed{nullptr};
    QLabel* m_latest{nullptr};
    QLabel* m_notes{nullptr};
    QLabel* m_size{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_install_note{nullptr};
    QPushButton* m_check{nullptr};
    QPushButton* m_download{nullptr};
    QPushButton* m_install{nullptr};
    QCheckBox* m_auto{nullptr};
};

#endif // B3COIN_QT_UPDATEPANEL_H

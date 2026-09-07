// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/test/b3stakesettingstests.h>

#include <qt/b3settingspage.h>
#include <qt/b3shell.h>
#include <qt/b3stakepage.h>
#include <qt/b3validatorcontroller.h>
#include <univalue.h>

#include <QAction>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

void B3StakeSettingsTests::stakePageDisablesActionsWithoutWallet()
{
    B3StakePage page;

    // No wallet: the page says what is missing and exposes no executable
    // validator, staking, mining, backup, or copy action.
    const auto* no_wallet = page.findChild<QLabel*>(QStringLiteral("stakeNoWallet"));
    QVERIFY(no_wallet != nullptr);
    QVERIFY(no_wallet->text().contains(QStringLiteral("wallet"), Qt::CaseInsensitive));
    QVERIFY(!no_wallet->isHidden());

    for (const char* name : {"stakeBindFinality", "stakeCreate", "stakeStartStop",
                             "stakeCorridorMining", "stakeBackupWallet"}) {
        const auto* button = page.findChild<QPushButton*>(QLatin1String(name));
        QVERIFY2(button != nullptr, name);
        QVERIFY2(!button->isEnabled(), name);
    }
    for (const char* name : {"stakeCopyValidator", "stakeCopyBls"}) {
        const auto* button = page.findChild<QPushButton*>(QLatin1String(name));
        QVERIFY2(button != nullptr, name);
        QVERIFY2(!button->isEnabled(), name);
    }

    // Detaching again is a no-op, not a crash.
    page.setWalletModel(nullptr);
}

void B3StakeSettingsTests::stakingRequiresCurrentSnapshotKey()
{
    UniValue finality;
    // Rotation or revocation cannot prevent an unlocked wallet from using
    // the original key still frozen into its current validator snapshot.
    QVERIFY(finality.read(R"({
        "validator_set":{"member":true},
        "binding":{"bound":true,"revoked":true,"key_is_ours":false},
        "signing":{"snapshot_keys":[
            {"current":false,"key_available":false},
            {"current":true,"key_available":true}
        ]}
    })"));
    QVERIFY(B3ValidatorController::FinalityStartError(finality).isEmpty());

    // Neither a future/previous key nor the latest bound key may stand in
    // for an unavailable current snapshot key.
    QVERIFY(finality.read(R"({
        "validator_set":{"member":true},
        "binding":{"bound":true,"revoked":false,"key_is_ours":true},
        "signing":{"snapshot_keys":[
            {"current":false,"key_available":true},
            {"current":true,"key_available":false}
        ]}
    })"));
    QVERIFY(B3ValidatorController::FinalityStartError(finality).contains(QStringLiteral("original key")));
    QVERIFY(finality.read(R"({
        "validator_set":{"member":true},
        "binding":{"bound":true,"revoked":false,"key_is_ours":true},
        "signing":{"snapshot_keys":[{"current":false,"key_available":true}]}
    })"));
    QVERIFY(!B3ValidatorController::FinalityStartError(finality).isEmpty());

    // Before the validator becomes a current member, retain the existing
    // confirmed-binding startup path and its missing-key refusal.
    QVERIFY(finality.read(R"({
        "validator_set":{"member":false},
        "binding":{"bound":true,"revoked":false,"key_is_ours":true}
    })"));
    QVERIFY(B3ValidatorController::FinalityStartError(finality).isEmpty());
    QVERIFY(finality.read(R"({
        "validator_set":{"member":false},
        "binding":{"bound":true,"revoked":false,"key_is_ours":false}
    })"));
    QVERIFY(!B3ValidatorController::FinalityStartError(finality).isEmpty());
    QVERIFY(finality.read(R"({
        "validator_set":{"member":false},
        "binding":{"bound":false}
    })"));
    QVERIFY(!B3ValidatorController::FinalityStartError(finality).isEmpty());
}

void B3StakeSettingsTests::settingsPageRoutesToExistingDialogs()
{
    B3SettingsPage page;
    qRegisterMetaType<OptionsDialog::Tab>("OptionsDialog::Tab");
    QSignalSpy spy(&page, &B3SettingsPage::openOptionsRequested);

    page.findChild<QPushButton*>("settingsOpenMain")->click();
    page.findChild<QPushButton*>("settingsOpenNetwork")->click();

    QCOMPARE(spy.count(), 2);
    QCOMPARE(qvariant_cast<OptionsDialog::Tab>(spy.at(0).at(0)), OptionsDialog::TAB_MAIN);
    QCOMPARE(qvariant_cast<OptionsDialog::Tab>(spy.at(1).at(0)), OptionsDialog::TAB_NETWORK);
}

void B3StakeSettingsTests::settingsPageMirrorsWalletActions()
{
    B3SettingsPage page;
    QAction encrypt(QStringLiteral("Encrypt Wallet…"), &page);
    QAction unlock(QStringLiteral("Unlock Wallet…"), &page);
    QAction lock(QStringLiteral("Lock Wallet"), &page);
    encrypt.setEnabled(false);
    lock.setEnabled(false);
    page.setWalletActions({&encrypt, &unlock, &lock, nullptr});

    QPushButton* encrypt_button{nullptr};
    QPushButton* unlock_button{nullptr};
    QPushButton* lock_button{nullptr};
    for (QPushButton* candidate : page.findChildren<QPushButton*>()) {
        if (candidate->text() == encrypt.text()) encrypt_button = candidate;
        if (candidate->text() == unlock.text()) unlock_button = candidate;
        if (candidate->text() == lock.text()) lock_button = candidate;
    }
    QVERIFY(encrypt_button != nullptr);
    QVERIFY(unlock_button != nullptr);
    QVERIFY(lock_button != nullptr);
    QVERIFY(!encrypt_button->isEnabled());
    QVERIFY(unlock_button->isEnabled());
    QVERIFY(!lock_button->isEnabled());

    // Enabled state follows the existing action.
    encrypt.setEnabled(true);
    QVERIFY(encrypt_button->isEnabled());

    // Clicking triggers the existing action — behavior stays with it.
    QSignalSpy unlock_spy(&unlock, &QAction::triggered);
    unlock_button->click();
    QCOMPARE(unlock_spy.count(), 1);

    // The same button surface can switch safely from unlock to relock.
    unlock.setEnabled(false);
    lock.setEnabled(true);
    QVERIFY(!unlock_button->isEnabled());
    QVERIFY(lock_button->isEnabled());
}

void B3StakeSettingsTests::shellShowsInstalledSettingsPage()
{
    B3Shell shell;
    QVERIFY(!shell.hasSettingsPage());

    auto* settings = new B3SettingsPage;
    shell.setSettingsPage(settings);
    QVERIFY(shell.hasSettingsPage());

    shell.showPage(B3Page::Settings);
    QCOMPARE(shell.sidebar()->currentPage(), B3Page::Settings);
    QVERIFY(settings->isVisibleTo(&shell) || settings->parent() != nullptr);
}

void B3StakeSettingsTests::settingsActionsTrackVisibilityIconsAndReplacement()
{
    B3SettingsPage page;
    // Use a registered resource, and verify the source before testing the
    // mirror. There is no ":/icons/filesave" alias in bitcoin.qrc.
    const QIcon backup_icon{QStringLiteral(":/icons/export")};
    QVERIFY(!backup_icon.isNull());
    QAction backup(backup_icon, QStringLiteral("Backup"), &page);
    backup.setToolTip(QStringLiteral("Back up this wallet"));
    page.setWalletActions({&backup});
    const auto buttons = [&] {
        QList<QPushButton*> result;
        for (auto* button : page.findChildren<QPushButton*>()) {
            if (button->property("b3WalletAction").toBool()) result.push_back(button);
        }
        return result;
    };
    QCOMPARE(buttons().size(), 1);
    auto* button = buttons().front();
    QVERIFY(!button->icon().isNull());
    QCOMPARE(button->icon().cacheKey(), backup.icon().cacheKey());
    const QIcon changed_icon{QStringLiteral(":/icons/lock_closed")};
    QVERIFY(!changed_icon.isNull());
    backup.setIcon(changed_icon);
    QCOMPARE(button->icon().cacheKey(), changed_icon.cacheKey());
    QCOMPARE(button->toolTip(), backup.toolTip());
    backup.setVisible(false);
    QVERIFY(button->isHidden());
    backup.setVisible(true);
    QVERIFY(!button->isHidden());
    page.setWalletActions({&backup});
    QCOMPARE(buttons().size(), 1);

    auto* transient = new QAction(QStringLiteral("Temporary"));
    page.setWalletActions({transient});
    button = buttons().front();
    delete transient;
    QVERIFY(button->isHidden());
    QVERIFY(!button->isEnabled());
    page.setWalletActions({});
    QVERIFY(buttons().isEmpty());
}

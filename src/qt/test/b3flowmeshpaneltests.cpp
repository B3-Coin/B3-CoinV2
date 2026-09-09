// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/b3assetsenddialog.h>
#include <qt/b3flowmeshpanel.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <test/util/setup_common.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};
const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};
const std::function<std::string()> G_TEST_GET_FULL_NAME{[] { return "b3_flowmesh_panel"; }};

class B3FlowMeshPanelTests : public QObject
{
    Q_OBJECT

    std::unique_ptr<TestingSetup> m_setup;
    std::unique_ptr<interfaces::Node> m_node;
    std::unique_ptr<interfaces::WalletLoader> m_loader;
    std::unique_ptr<OptionsModel> m_options;
    std::unique_ptr<ClientModel> m_client;
    std::unique_ptr<const PlatformStyle> m_style;
    std::shared_ptr<wallet::CWallet> m_first_wallet;
    std::shared_ptr<wallet::CWallet> m_second_wallet;
    std::unique_ptr<WalletModel> m_first;
    std::unique_ptr<WalletModel> m_second;

    static SecureString Passphrase() { return SecureString{"offline FN panel fixture passphrase"}; }

    std::shared_ptr<wallet::CWallet> MakeWallet(const std::string& name)
    {
        auto wallet = std::make_shared<wallet::CWallet>(
            m_setup->m_node.chain.get(), name, wallet::CreateMockableWalletDatabase());
        LOCK(wallet->cs_wallet);
        wallet->SetLastBlockProcessed(0, m_setup->m_node.chain->getBlockHash(0));
        wallet->m_keypool_size = 1;
        wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
        wallet->SetupDescriptorScriptPubKeyMans();
        return wallet;
    }

    std::unique_ptr<WalletModel> MakeModel(const std::shared_ptr<wallet::CWallet>& wallet)
    {
        return std::make_unique<WalletModel>(interfaces::MakeWallet(*m_loader->context(), wallet),
                                             *m_client, m_style.get());
    }

    void ObserveArmableStatus(B3FlowMeshPanel& panel)
    {
        panel.setWalletModel(m_first.get());
        panel.cancelAndWait();
        // Only the previously observed public status is synthetic. No FN keys
        // are created or armed, and every control path is cancelled before RPC.
        B3FlowMeshOperator::Status state;
        state.available = true;
        state.fingerprint = QString(64, QLatin1Char('1'));
        state.wallet_keys = {QString(96, QLatin1Char('2'))};
        panel.m_state = std::move(state);
        panel.m_cancel->store(false);
        panel.showStatus();
    }

    static B3AssetRecord FnRecord()
    {
        B3AssetRecord fn;
        fn.asset_id = QString(64, QLatin1Char('3'));
        fn.ticker = QStringLiteral("FN");
        fn.is_fn = true;
        fn.precision_known = true;
        fn.decimals = 0;
        fn.status = B3AssetRecord::Status::Active;
        fn.confirmed = 1;
        return fn;
    }

private Q_SLOTS:
    void initTestCase()
    {
        Q_INIT_RESOURCE(bitcoin);
        // Isolated genesis-only regtest and mock wallet databases. No connman,
        // sockets, funding, mining, FN activation, signing or broadcasting.
        m_setup = std::make_unique<TestingSetup>(ChainType::REGTEST,
            TestOpts{.extra_args = {"-keypool=1"}, .setup_net = false});
        m_node = interfaces::MakeNode(m_setup->m_node);
        m_loader = interfaces::MakeWalletLoader(*m_setup->m_node.chain, *m_setup->m_node.args);
        m_setup->m_node.wallet_loader = m_loader.get();
        m_options = std::make_unique<OptionsModel>(*m_node);
        bilingual_str error;
        QVERIFY2(m_options->Init(error), error.original.c_str());
        m_client = std::make_unique<ClientModel>(*m_node, m_options.get());
        m_style.reset(PlatformStyle::instantiate("other"));
        QVERIFY(m_style != nullptr);
        m_first_wallet = MakeWallet("offline-fn-first");
        m_second_wallet = MakeWallet("offline-fn-second");
        {
            LOCK(m_loader->context()->wallets_mutex);
            m_loader->context()->wallets.push_back(m_first_wallet);
            m_loader->context()->wallets.push_back(m_second_wallet);
        }
        m_first = MakeModel(m_first_wallet);
        m_second = MakeModel(m_second_wallet);
        QVERIFY(m_first->setWalletEncrypted(Passphrase()));
    }

    void init()
    {
        QVERIFY(m_first->setWalletLocked(true));
        QVERIFY(m_first_wallet->IsLocked());
        QVERIFY(!m_second_wallet->IsLocked());
    }

    void nullWalletNeverEnablesControls()
    {
        B3FlowMeshPanel panel;
        panel.setFnAsset(FnRecord());
        for (const auto* button : {panel.m_bind, panel.m_arm, panel.m_disarm, panel.m_refresh}) {
            QVERIFY(!button->isEnabled());
        }
        QVERIFY(!panel.m_timer->isActive());
        panel.cancelAndWait();
        panel.setWalletModel(nullptr);
        panel.cancelAndWait();
        QVERIFY(panel.m_thread == nullptr);
        QVERIFY(!panel.m_timer->isActive());
    }

    void attachStartsTimerAndDetachDrainsReadOnlyWorker()
    {
        B3FlowMeshPanel panel;
        panel.setWalletModel(m_first.get());
        QVERIFY(panel.m_timer->isActive());
        panel.setWalletModel(nullptr);
        QVERIFY(panel.m_thread == nullptr);
        QVERIFY(!panel.m_timer->isActive());
        QVERIFY(panel.m_wallet.isNull());
        QVERIFY(!panel.m_backend);
        QCoreApplication::processEvents();
        QVERIFY(panel.m_thread == nullptr); // No stale finished callback restarts it.
        QVERIFY(!panel.m_refresh->isEnabled());
        QVERIFY(m_first_wallet->IsLocked());
    }

    void walletSwitchRejectsConfirmationWithoutUnlocking()
    {
        B3FlowMeshPanel panel;
        ObserveArmableStatus(panel);
        QSignalSpy unlock(m_first.get(), &WalletModel::requireUnlock);
        bool observed{false};
        QTimer::singleShot(0, &panel, [&] {
            observed = panel.m_confirmation &&
                panel.m_confirmation->defaultButton() == panel.m_confirmation->button(QMessageBox::Cancel);
            panel.setWalletModel(m_second.get());
        });
        panel.control(B3FlowMeshPanel::Operation::Arm);
        QVERIFY(observed);
        QCOMPARE(unlock.count(), 0);
        QCOMPARE(panel.m_wallet.data(), m_second.get());
        QVERIFY(panel.m_confirmation.isNull());
        panel.cancelAndWait();
        QVERIFY(m_first_wallet->IsLocked());
        QVERIFY(!m_second_wallet->IsLocked());
    }

    void parentDeletionDuringConfirmationIsSafe()
    {
        QPointer<QWidget> parent{new QWidget};
        QPointer<B3FlowMeshPanel> panel{new B3FlowMeshPanel{parent}};
        ObserveArmableStatus(*panel);
        QSignalSpy unlock(m_first.get(), &WalletModel::requireUnlock);
        bool observed{false};
        QTimer::singleShot(0, this, [&] {
            observed = panel && panel->m_confirmation;
            delete parent.data();
        });
        panel->control(B3FlowMeshPanel::Operation::Arm);
        QVERIFY(observed);
        QVERIFY(parent.isNull());
        QVERIFY(panel.isNull());
        QCOMPARE(unlock.count(), 0);
        QVERIFY(m_first_wallet->IsLocked());
    }

    void walletSwitchInsideSuccessfulUnlockRelocksOnlyCapturedWallet()
    {
        B3FlowMeshPanel panel;
        ObserveArmableStatus(panel);
        bool prompt_seen{false};
        const auto connection = connect(m_first.get(), &WalletModel::requireUnlock, this, [&] {
            prompt_seen = true;
            QVERIFY(m_first->setWalletLocked(false, Passphrase()));
            QVERIFY(!m_first_wallet->IsLocked());
            panel.setWalletModel(m_second.get());
        });
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) panel.m_confirmation->done(QMessageBox::Yes);
        });
        panel.control(B3FlowMeshPanel::Operation::Arm);
        disconnect(connection);
        QVERIFY(prompt_seen);
        QCOMPARE(panel.m_wallet.data(), m_second.get());
        panel.cancelAndWait();
        QVERIFY(m_first_wallet->IsLocked());
        QVERIFY(!m_second_wallet->IsLocked());
        QVERIFY(!panel.m_unlock);
        QVERIFY(panel.m_security_warning.isEmpty());
    }

    void deferredRelockUsesOldBackendNotNewSelection()
    {
        B3FlowMeshPanel panel;
        panel.setWalletModel(m_second.get());
        panel.cancelAndWait();
        QVERIFY(m_first->setWalletLocked(false, Passphrase()));
        panel.m_restore_locked = true;
        panel.m_relock_backend = interfaces::MakeWallet(*m_loader->context(), m_first_wallet);
        panel.m_relock_wallet_name = m_first->getDisplayName();
        QVERIFY(panel.restoreSpendingLock());
        QVERIFY(m_first_wallet->IsLocked());
        QVERIFY(!m_second_wallet->IsLocked());
        QVERIFY(!panel.m_relock_backend);
    }

    void failedRelockWarningRemainsVisibleAndDisablesControls()
    {
        B3FlowMeshPanel panel;
        ObserveArmableStatus(panel);
        panel.m_restore_locked = true;
        panel.m_relock_wallet_name = QStringLiteral("captured old wallet");
        // Missing restoration handle is a deterministic failure, without a
        // mock lock implementation or leaving an encrypted wallet unlocked.
        QVERIFY(!panel.restoreSpendingLock());
        panel.showStatus();
        QVERIFY(panel.m_message->text().contains(QStringLiteral("SECURITY")));
        QVERIFY(panel.m_message->text().contains(QStringLiteral("captured old wallet")));
        for (const auto* button : {panel.m_bind, panel.m_arm, panel.m_disarm, panel.m_refresh}) {
            QVERIFY(!button->isEnabled());
        }
        panel.setWalletModel(m_second.get());
        panel.cancelAndWait();
        QVERIFY(panel.m_message->text().contains(QStringLiteral("captured old wallet")));
        QVERIFY(!m_second_wallet->IsLocked());
    }

    void fnBindingCancelInsideUnlockRelocksWithoutPreparing()
    {
        B3AssetSendDialog dialog{m_first.get(), FnRecord(), nullptr, true};
        QSignalSpy submitted(&dialog, &B3AssetSendDialog::transactionSubmitted);
        auto* action{dialog.findChild<QPushButton*>(QStringLiteral("assetSendAction"))};
        QVERIFY(action && action->isEnabled());
        bool prompt_seen{false};
        const auto connection = connect(m_first.get(), &WalletModel::requireUnlock, this, [&] {
            prompt_seen = true;
            QVERIFY(m_first->setWalletLocked(false, Passphrase()));
            dialog.cancelAndWait();
        });
        action->click();
        disconnect(connection);
        QVERIFY(prompt_seen);
        QCOMPARE(submitted.count(), 0);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(m_first_wallet->IsLocked());
    }

    void fnBindingThrowingUnlockRelocksWithoutPreparing()
    {
        B3AssetSendDialog dialog{m_first.get(), FnRecord(), nullptr, true};
        auto* action{dialog.findChild<QPushButton*>(QStringLiteral("assetSendAction"))};
        QVERIFY(action && action->isEnabled());
        const auto connection = connect(m_first.get(), &WalletModel::requireUnlock, this, [&] {
            if (!m_first->setWalletLocked(false, Passphrase())) throw std::runtime_error{"fixture unlock failed"};
            throw std::runtime_error{"offline prompt failure after unlocking"};
        });
        action->click();
        disconnect(connection);
        QVERIFY(m_first_wallet->IsLocked());
        const auto* status{dialog.findChild<QLabel*>(QStringLiteral("assetSendStatus"))};
        QVERIFY(status && status->text().contains(QStringLiteral("Nothing was submitted")));
    }

    void cleanupTestCase()
    {
        m_first.reset();
        m_second.reset();
        if (m_loader) WITH_LOCK(m_loader->context()->wallets_mutex, m_loader->context()->wallets.clear());
        m_first_wallet.reset();
        m_second_wallet.reset();
        m_client.reset();
        m_options.reset();
        if (m_setup) m_setup->m_node.wallet_loader = nullptr;
        m_loader.reset();
        m_node.reset();
        m_setup.reset();
    }
};

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "minimal");
    QApplication app(argc, argv);
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    QCoreApplication::setOrganizationName(QStringLiteral("B3HiveIsolatedTests"));
    QCoreApplication::setApplicationName(QStringLiteral("flowmesh-panel"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    B3FlowMeshPanelTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "b3flowmeshpaneltests.moc"

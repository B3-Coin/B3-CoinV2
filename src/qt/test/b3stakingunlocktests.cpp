// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <common/signmessage.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/askpassphrasedialog.h>
#include <qt/b3stakepage.h>
#include <qt/b3theme.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <test/util/setup_common.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};
const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};
const std::function<std::string()> G_TEST_GET_FULL_NAME{[] { return "b3_staking_unlock"; }};

namespace {
constexpr auto TEST_PASSPHRASE = "isolated staking unlock test passphrase";

SecureString Passphrase()
{
    return SecureString{TEST_PASSPHRASE};
}
} // namespace

class B3StakingUnlockTests : public QObject
{
    Q_OBJECT

    std::unique_ptr<TestingSetup> m_setup;
    std::unique_ptr<interfaces::Node> m_node;
    std::unique_ptr<interfaces::WalletLoader> m_loader;
    std::unique_ptr<OptionsModel> m_options;
    std::unique_ptr<ClientModel> m_client;
    std::unique_ptr<const PlatformStyle> m_style;
    std::shared_ptr<wallet::CWallet> m_wallet;
    std::unique_ptr<WalletModel> m_model;
    std::optional<PKHash> m_address;

    std::shared_ptr<wallet::CWallet> MakeWallet(bool private_keys = true)
    {
        auto wallet = std::make_shared<wallet::CWallet>(
            m_setup->m_node.chain.get(), "isolated-staking-unlock",
            wallet::CreateMockableWalletDatabase());
        LOCK(wallet->cs_wallet);
        wallet->m_keypool_size = 1;
        wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
        if (private_keys) {
            wallet->SetupDescriptorScriptPubKeyMans();
        } else {
            wallet->SetWalletFlag(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS);
        }
        return wallet;
    }

    std::unique_ptr<WalletModel> MakeModel(const std::shared_ptr<wallet::CWallet>& wallet)
    {
        return std::make_unique<WalletModel>(
            interfaces::MakeWallet(*m_loader->context(), wallet), *m_client,
            m_style.get());
    }

    SigningResult SignMessage()
    {
        std::string signature;
        return m_model->wallet().signMessage(
            "offline staking-unlock regression; not a transaction", *m_address,
            signature);
    }

private Q_SLOTS:
    void initTestCase()
    {
        Q_INIT_RESOURCE(bitcoin);
        B3Theme::apply(*qApp);
        // Genesis-only regtest, in-memory wallet DB, no connman or sockets.
        // Never initialize from the user's datadir or mine any blocks.
        m_setup = std::make_unique<TestingSetup>(
            ChainType::REGTEST, TestOpts{.extra_args = {"-keypool=1"}, .setup_net = false});
        m_node = interfaces::MakeNode(m_setup->m_node);
        m_loader = interfaces::MakeWalletLoader(*m_setup->m_node.chain, *m_setup->m_node.args);
        m_setup->m_node.wallet_loader = m_loader.get();
        m_options = std::make_unique<OptionsModel>(*m_node);
        bilingual_str error;
        QVERIFY2(m_options->Init(error), error.original.c_str());
        m_client = std::make_unique<ClientModel>(*m_node, m_options.get());
        m_style.reset(PlatformStyle::instantiate("other"));
        QVERIFY(m_style != nullptr);
        m_wallet = MakeWallet();
        m_model = MakeModel(m_wallet);
        const auto address = m_model->wallet().getNewDestination(OutputType::LEGACY, "offline-test");
        QVERIFY(address.has_value());
        const auto* pkhash = std::get_if<PKHash>(&*address);
        QVERIFY(pkhash != nullptr);
        m_address = *pkhash;
        QCOMPARE(SignMessage(), SigningResult::OK);
        QVERIFY(m_model->setWalletEncrypted(Passphrase()));
        QCOMPARE(m_model->getEncryptionStatus(), WalletModel::Locked);
        QCOMPARE(SignMessage(), SigningResult::PRIVATE_KEY_NOT_AVAILABLE);
    }

    void init()
    {
        QVERIFY(m_model->setWalletLocked(true));
        QCOMPARE(m_model->getEncryptionStatus(), WalletModel::Locked);
    }

    void lockedSuccessUsesStakingPromptAndRelocksRealSigning()
    {
        QSignalSpy general(m_model.get(), &WalletModel::requireUnlock);
        QSignalSpy staking(m_model.get(), &WalletModel::requireUnlockForStaking);
        const auto connection = connect(m_model.get(), &WalletModel::requireUnlockForStaking,
                                        this, [this] {
            AskPassphraseDialog dialog{AskPassphraseDialog::UnlockStaking, nullptr};
            dialog.setModel(m_model.get());
            QCOMPARE(dialog.windowTitle(), QStringLiteral("Unlock for staking only"));
            auto* warning = dialog.findChild<QLabel*>(QStringLiteral("warningLabel"));
            auto* passphrase = dialog.findChild<QLineEdit*>(QStringLiteral("passEdit1"));
            QVERIFY(warning != nullptr);
            QVERIFY(passphrase != nullptr);
            QVERIFY(warning->text().contains(QStringLiteral("lock again for spending")));
            QCOMPARE(passphrase->echoMode(), QLineEdit::Password);
            passphrase->setText(QString::fromUtf8(TEST_PASSPHRASE));
            dialog.accept();
            QCOMPARE(dialog.result(), int(QDialog::Accepted));
        });
        {
            auto unlock = m_model->requestUnlock(WalletModel::UnlockPurpose::StakingOnly);
            QVERIFY(unlock.isValid());
            QCOMPARE(m_model->getEncryptionStatus(), WalletModel::Unlocked);
            QCOMPARE(SignMessage(), SigningResult::OK);
        }
        disconnect(connection);
        QCOMPARE(staking.count(), 1);
        QCOMPARE(general.count(), 0);
        QCOMPARE(m_model->getEncryptionStatus(), WalletModel::Locked);
        QCOMPARE(SignMessage(), SigningResult::PRIVATE_KEY_NOT_AVAILABLE);
    }

    void cancelledStakingUnlockStaysLocked()
    {
        QSignalSpy staking(m_model.get(), &WalletModel::requireUnlockForStaking);
        // No signal receiver accepts an unlock: identical to dialog cancel.
        {
            auto unlock = m_model->requestUnlock(WalletModel::UnlockPurpose::StakingOnly);
            QVERIFY(!unlock.isValid());
        }
        QCOMPARE(staking.count(), 1);
        QCOMPARE(m_model->getEncryptionStatus(), WalletModel::Locked);
        QCOMPARE(SignMessage(), SigningResult::PRIVATE_KEY_NOT_AVAILABLE);
    }

    void wrongPassphraseStaysLocked()
    {
        bool attempted{false};
        const auto connection = connect(m_model.get(), &WalletModel::requireUnlockForStaking,
                                        this, [this, &attempted] {
            attempted = true;
            QVERIFY(!m_model->setWalletLocked(false, SecureString{"incorrect test passphrase"}));
        });
        {
            auto unlock = m_model->requestUnlock(WalletModel::UnlockPurpose::StakingOnly);
            QVERIFY(!unlock.isValid());
        }
        disconnect(connection);
        QVERIFY(attempted);
        QCOMPARE(m_model->getEncryptionStatus(), WalletModel::Locked);
        QCOMPARE(SignMessage(), SigningResult::PRIVATE_KEY_NOT_AVAILABLE);
    }

    void alreadyUnlockedStakingContextRelocksEvenOnFailure()
    {
        QVERIFY(m_model->setWalletLocked(false, Passphrase()));
        QSignalSpy general(m_model.get(), &WalletModel::requireUnlock);
        QSignalSpy staking(m_model.get(), &WalletModel::requireUnlockForStaking);
        try {
            auto unlock = m_model->requestUnlock(WalletModel::UnlockPurpose::StakingOnly);
            QVERIFY(unlock.isValid());
            QCOMPARE(SignMessage(), SigningResult::OK);
            throw std::runtime_error("synthetic staking-start failure");
        } catch (const std::runtime_error&) {
        }
        QCOMPARE(staking.count(), 0);
        QCOMPARE(general.count(), 0);
        QCOMPARE(m_model->getEncryptionStatus(), WalletModel::Locked);
        QCOMPARE(SignMessage(), SigningResult::PRIVATE_KEY_NOT_AVAILABLE);
    }

    void synchronousPromptExceptionRelocksBeforeContextExists()
    {
        bool caught{false};
        const auto connection = connect(m_model.get(), &WalletModel::requireUnlockForStaking,
                                        this, [this] {
            QVERIFY(m_model->setWalletLocked(false, Passphrase()));
            QCOMPARE(SignMessage(), SigningResult::OK);
            throw std::runtime_error("synthetic unlock-prompt failure");
        });
        try {
            auto unlock = m_model->requestUnlock(WalletModel::UnlockPurpose::StakingOnly);
            Q_UNUSED(unlock);
        } catch (const std::runtime_error&) {
            caught = true;
        }
        disconnect(connection);
        QVERIFY(caught);
        QCOMPARE(m_model->getEncryptionStatus(), WalletModel::Locked);
        QCOMPARE(SignMessage(), SigningResult::PRIVATE_KEY_NOT_AVAILABLE);
    }

    void dialogUnlockExceptionRelocksBeforeShowingError()
    {
        bool injected{false};
        bool error_seen{false};
        // Throw only after the real wallet has decrypted, not on its subsequent
        // locked notification. This exercises a partial-success unlock failure.
        auto handler = m_model->wallet().handleStatusChanged([this, &injected] {
            if (!injected && !m_model->wallet().isLocked()) {
                injected = true;
                throw std::runtime_error("synthetic post-decryption failure");
            }
        });
        const auto connection = connect(m_model.get(), &WalletModel::requireUnlockForStaking,
                                        this, [this, &error_seen] {
            AskPassphraseDialog dialog{AskPassphraseDialog::UnlockStaking, nullptr};
            dialog.setModel(m_model.get());
            auto* passphrase = dialog.findChild<QLineEdit*>(QStringLiteral("passEdit1"));
            QVERIFY(passphrase != nullptr);
            passphrase->setText(QString::fromUtf8(TEST_PASSPHRASE));
            QTimer close_error;
            close_error.setSingleShot(true);
            connect(&close_error, &QTimer::timeout, this, [this, &error_seen] {
                for (QWidget* widget : QApplication::topLevelWidgets()) {
                    if (auto* message = qobject_cast<QMessageBox*>(widget)) {
                        error_seen = true;
                        const auto state = m_model->getEncryptionStatus();
                        const auto signing = SignMessage();
                        message->accept();
                        QCOMPARE(state, WalletModel::Locked);
                        QCOMPARE(signing, SigningResult::PRIVATE_KEY_NOT_AVAILABLE);
                    }
                }
            });
            close_error.start(0);
            dialog.accept();
            QCOMPARE(dialog.result(), int(QDialog::Rejected));
        });
        {
            auto unlock = m_model->requestUnlock(WalletModel::UnlockPurpose::StakingOnly);
            QVERIFY(!unlock.isValid());
        }
        disconnect(connection);
        handler->disconnect();
        QVERIFY(injected);
        QVERIFY(error_seen);
        QCOMPARE(m_model->getEncryptionStatus(), WalletModel::Locked);
        QCOMPARE(SignMessage(), SigningResult::PRIVATE_KEY_NOT_AVAILABLE);
    }

    void generalContextPreservesPreexistingUnlock()
    {
        QVERIFY(m_model->setWalletLocked(false, Passphrase()));
        QSignalSpy general(m_model.get(), &WalletModel::requireUnlock);
        QSignalSpy staking(m_model.get(), &WalletModel::requireUnlockForStaking);
        {
            auto unlock = m_model->requestUnlock();
            QVERIFY(unlock.isValid());
        }
        QCOMPARE(m_model->getEncryptionStatus(), WalletModel::Unlocked);
        QCOMPARE(SignMessage(), SigningResult::OK);
        QCOMPARE(general.count(), 0);
        QCOMPARE(staking.count(), 0);
        QVERIFY(m_model->setWalletLocked(true));
    }

    void unencryptedAndWatchOnlyNeverRequestAPassword()
    {
        for (const bool private_keys : {true, false}) {
            const auto wallet = MakeWallet(private_keys);
            const auto model = MakeModel(wallet);
            const auto expected = private_keys ? WalletModel::Unencrypted : WalletModel::NoKeys;
            QCOMPARE(model->getEncryptionStatus(), expected);
            QSignalSpy general(model.get(), &WalletModel::requireUnlock);
            QSignalSpy staking(model.get(), &WalletModel::requireUnlockForStaking);
            {
                auto unlock = model->requestUnlock(WalletModel::UnlockPurpose::StakingOnly);
                QVERIFY(unlock.isValid());
            }
            QCOMPARE(model->getEncryptionStatus(), expected);
            QVERIFY(!model->wallet().isCrypted());
            QCOMPARE(general.count(), 0);
            QCOMPARE(staking.count(), 0);
        }
    }

    void stakingLabelsAreHonestAboutSpendingProtection_data()
    {
        QTest::addColumn<int>("encryption");
        QTest::addColumn<bool>("running");
        QTest::addColumn<bool>("own_wallet");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("lock_text");
        QTest::newRow("locked-stopped") << int(WalletModel::Locked) << false << false << true
            << QStringLiteral("Unlock for staking only") << QStringLiteral("Spending locked");
        QTest::newRow("unlocked-stopped") << int(WalletModel::Unlocked) << false << false << true
            << QStringLiteral("Start staking && lock wallet") << QStringLiteral("Unlocked for spending");
        QTest::newRow("unencrypted-stopped") << int(WalletModel::Unencrypted) << false << false << true
            << QStringLiteral("Start staking") << QStringLiteral("Not encrypted");
        QTest::newRow("watch-only") << int(WalletModel::NoKeys) << false << false << true
            << QStringLiteral("Start staking") << QStringLiteral("Watch-only");
        QTest::newRow("own-staking-locked") << int(WalletModel::Locked) << true << true << true
            << QStringLiteral("Stop staking") << QStringLiteral("Staking active — spending locked");
        QTest::newRow("own-staking-unlocked") << int(WalletModel::Unlocked) << true << true << true
            << QStringLiteral("Stop staking") << QStringLiteral("Staking active — spending unlocked");
        QTest::newRow("own-staking-unencrypted") << int(WalletModel::Unencrypted) << true << true << true
            << QStringLiteral("Stop staking") << QStringLiteral("Staking active — wallet not encrypted");
        QTest::newRow("other-wallet-staking") << int(WalletModel::Locked) << true << false << true
            << QStringLiteral("Another wallet is staking") << QStringLiteral("Spending locked");
        QTest::newRow("unverified-own-status") << int(WalletModel::Locked) << true << true << false
            << QStringLiteral("Stop staking") << QStringLiteral("Spending locked");
    }

    void stakingLabelsAreHonestAboutSpendingProtection()
    {
        QFETCH(int, encryption);
        QFETCH(bool, running);
        QFETCH(bool, own_wallet);
        QFETCH(bool, valid);
        QFETCH(QString, action);
        QFETCH(QString, lock_text);
        B3ValidatorStatus status;
        status.valid = valid;
        status.staking_running = running;
        status.staking_uses_this_wallet = own_wallet;
        QCOMPARE(B3StakePage::StakingActionText(encryption, status), action);
        QCOMPARE(B3StakePage::WalletLockText(encryption, status), lock_text);

        // Render the real page widgets with public presentation-helper output.
        // No model is attached: no controller polling, RPC, keys or staking.
        B3StakePage page;
        auto* lock = page.findChild<QLabel*>(QStringLiteral("stakeLockState"));
        auto* start_stop = page.findChild<QPushButton*>(QStringLiteral("stakeStartStop"));
        QVERIFY(lock != nullptr);
        QVERIFY(start_stop != nullptr);
        lock->setText(B3StakePage::WalletLockText(encryption, status));
        start_stop->setText(B3StakePage::StakingActionText(encryption, status));
        page.resize(1000, 900);
        page.ensurePolished();
        QImage render(page.size(), QImage::Format_ARGB32_Premultiplied);
        render.fill(Qt::transparent);
        page.render(&render);
        QVERIFY(!render.isNull());
        QCOMPARE(lock->text(), lock_text);
        QCOMPARE(start_stop->text(), action);
    }

    void cleanupTestCase()
    {
        m_model.reset();
        m_wallet.reset();
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
    QCoreApplication::setApplicationName(QStringLiteral("staking-unlock"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    B3StakingUnlockTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "b3stakingunlocktests.moc"

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/b3flowmeshtradingpanel.h>
#include <qt/b3tradepage.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <test/util/setup_common.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QApplication>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <functional>
#include <memory>
#include <vector>

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};
const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};
const std::function<std::string()> G_TEST_GET_FULL_NAME{[] { return "b3_flowmesh_trading_panel"; }};

class B3FlowMeshTradingPanelTests : public QObject
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

private Q_SLOTS:
    void initTestCase()
    {
        Q_INIT_RESOURCE(bitcoin);
        // Genesis-only regtest: no connman, network sockets, funding or mining.
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
        m_wallet = std::make_shared<wallet::CWallet>(m_setup->m_node.chain.get(),
            "offline-trading-panel", wallet::CreateMockableWalletDatabase());
        {
            LOCK(m_wallet->cs_wallet);
            m_wallet->SetLastBlockProcessed(0, m_setup->m_node.chain->getBlockHash(0));
            m_wallet->m_keypool_size = 1;
            m_wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
            m_wallet->SetupDescriptorScriptPubKeyMans();
        }
        {
            LOCK(m_loader->context()->wallets_mutex);
            m_loader->context()->wallets.push_back(m_wallet);
        }
        m_model = std::make_unique<WalletModel>(
            interfaces::MakeWallet(*m_loader->context(), m_wallet), *m_client, m_style.get());
        QVERIFY(m_model->setWalletEncrypted(SecureString{"offline test passphrase"}));
        QVERIFY(m_model->setWalletLocked(true));
    }

    void noWalletRetainsNonSubmittingPreview()
    {
        B3TradePage page;
        page.setWalletModel(nullptr);
        QVERIFY(page.findChild<B3FlowMeshTradingPanel*>() == nullptr);
        const auto* submit{page.findChild<QPushButton*>(QStringLiteral("ticketSubmit"))};
        QVERIFY(submit != nullptr);
        QVERIFY(!submit->isEnabled());
    }

    void walletAttachAndDetachSelectRealPanel()
    {
        B3TradePage page;
        page.setWalletModel(m_model.get());
        const auto panel{page.findChild<B3FlowMeshTradingPanel*>()};
        QVERIFY(panel != nullptr);
        QVERIFY(!panel->isHidden());
        page.openFlowMeshAsset(QString(64, QLatin1Char('1')), false);
        page.setWalletModel(nullptr);
        QVERIFY(panel->isHidden());
        QVERIFY(m_wallet->IsLocked());
        QCoreApplication::processEvents();
        QVERIFY(panel->isHidden());
        page.setWalletModel(m_model.get());
        QCOMPARE(page.findChild<B3FlowMeshTradingPanel*>(), panel);
        QVERIFY(!panel->isHidden());
        page.setWalletModel(nullptr);
        QVERIFY(m_wallet->IsLocked());
    }

    void destructionDrainsReadOnlyWorker()
    {
        auto page{std::make_unique<B3TradePage>()};
        page->setWalletModel(m_model.get());
        QPointer<B3FlowMeshTradingPanel> panel{page->findChild<B3FlowMeshTradingPanel*>()};
        QVERIFY(panel);
        page.reset();
        QVERIFY(panel.isNull());
        QCoreApplication::processEvents();
        QVERIFY(m_wallet->IsLocked());
    }

    void cleanupTestCase()
    {
        m_model.reset();
        {
            LOCK(m_loader->context()->wallets_mutex);
            m_loader->context()->wallets.clear();
        }
        m_wallet.reset();
        m_client.reset();
        m_options.reset();
        m_setup->m_node.wallet_loader = nullptr;
        m_loader.reset();
        m_node.reset();
        m_setup.reset();
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("B3OfflineTests"));
    QCoreApplication::setApplicationName(QStringLiteral("FlowMeshTradingPanel"));
    B3FlowMeshTradingPanelTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "b3flowmeshtradingpaneltests.moc"

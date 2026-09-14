// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license.
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <modern/stake.h>
#include <qt/coincontroldialog.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsentry.h>
#include <qt/walletmodel.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QCheckBox>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};
const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};
const std::function<std::string()> G_TEST_GET_FULL_NAME{[] { return "b3_stake_coin_control"; }};

class B3StakeCoinControlTests : public QObject
{
    Q_OBJECT
    QTemporaryDir m_settings;
    std::unique_ptr<TestingSetup> m_setup;
    std::unique_ptr<interfaces::Node> m_node;
    std::unique_ptr<interfaces::WalletLoader> m_loader;
    std::unique_ptr<OptionsModel> m_options;
    std::unique_ptr<ClientModel> m_client;
    std::unique_ptr<const PlatformStyle> m_style;

    struct Fixture {
        std::shared_ptr<wallet::CWallet> wallet;
        std::unique_ptr<WalletModel> model;
        COutPoint ordinary, pending, active, locked, immature;
    };

    Fixture MakeFixture(bool watch_only = false, ClientModel* client = nullptr)
    {
        Fixture f;
        f.wallet = std::make_shared<wallet::CWallet>(m_setup->m_node.chain.get(),
            watch_only ? "test-watch-only-stake" : "test-owner-stake", wallet::CreateMockableWalletDatabase());
        CKey owner_key;
        owner_key.MakeNewKey(true);
        const CScript owner = GetScriptForDestination(PKHash(owner_key.GetPubKey()));
        std::array<unsigned char, 32> validator{};
        validator.fill(0x42); // Not the owner: knowing this identity gives no spending authority.
        const CScript stake = modern::MakeStakeScript(validator, owner);
        const auto genesis = m_setup->m_node.chain->getBlockHash(0);
        {
            LOCK(f.wallet->cs_wallet);
            f.wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
            if (watch_only) f.wallet->SetWalletFlag(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS);
            FlatSigningProvider provider;
            std::string error;
            auto descriptors = Parse("pkh(" + (watch_only ? HexStr(owner_key.GetPubKey()) : EncodeSecret(owner_key)) + ")", provider, error, false);
            assert(descriptors.size() == 1);
            wallet::WalletDescriptor descriptor(std::move(descriptors.front()), 0, 0, 0, 0);
            assert(f.wallet->AddWalletDescriptor(descriptor, provider, "", false));
            // Synthetic wallet metadata only. No claim these outputs exist in chainstate.
            // Chain-backed spending/reorg validation is covered by wallet_tests.
            f.wallet->SetLastBlockProcessed(100, genesis);
            const auto add = [&](const CScript& script, int height, uint32_t nonce, bool coinbase = false) {
                CMutableTransaction tx;
                tx.nLockTime = nonce;
                tx.vin.emplace_back();
                if (!coinbase) tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256::ONE), nonce};
                else tx.vin[0].scriptSig = CScript() << CScriptNum(1) << CScriptNum(nonce);
                tx.vout.emplace_back(333 * COIN, script);
                const auto ref = MakeTransactionRef(tx);
                assert(f.wallet->AddToWallet(ref, wallet::TxStateConfirmed{genesis, height, coinbase ? 0 : 1}));
                return COutPoint{ref->GetHash(), 0};
            };
            f.ordinary = add(owner, 80, 1);
            f.pending = add(stake, 100, 2);
            f.active = add(stake, 80, 3);
            f.locked = add(stake, 80, 4);
            f.immature = add(stake, 100, 5, true);
            f.wallet->LockCoin(f.locked, true);
        }
        f.model = std::make_unique<WalletModel>(interfaces::MakeWallet(*m_loader->context(), f.wallet), client ? *client : *m_client, m_style.get());
        return f;
    }

    static QTreeWidgetItem* Find(QTreeWidget* tree, const COutPoint& outpoint)
    {
        QTreeWidgetItemIterator it(tree);
        while (*it) {
            if ((*it)->data(3, Qt::UserRole).toString().toStdString() == outpoint.hash.GetHex()) return *it;
            ++it;
        }
        return nullptr;
    }

    static void AcceptInclude(QCheckBox* include)
    {
        QTimer::singleShot(0, [] {
            for (auto* widget : QApplication::topLevelWidgets())
                if (auto* box = qobject_cast<QMessageBox*>(widget)) box->button(QMessageBox::Yes)->click();
        });
        include->click();
        QTRY_VERIFY(include->isEnabled());
    }

    static void RebuildViewByChangingMode(CoinControlDialog& dialog)
    {
        auto* tree_mode = dialog.findChild<QRadioButton*>("radioTreeMode");
        auto* list_mode = dialog.findChild<QRadioButton*>("radioListMode");
        QVERIFY(tree_mode);
        QVERIFY(list_mode);
        // Mode is persisted between dialogs. Clicking an already-selected
        // radio button emits no toggle and does not refresh the inventory.
        auto* next_mode = tree_mode->isChecked() ? list_mode : tree_mode;
        QVERIFY(!next_mode->isChecked());
        next_mode->click();
        QVERIFY(next_mode->isChecked());
    }

private Q_SLOTS:
    void initTestCase()
    {
        Q_INIT_RESOURCE(bitcoin);
        QVERIFY(m_settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        QCoreApplication::setOrganizationName("B3IsolatedStakeTest");
        QCoreApplication::setApplicationName("CoinControl");
        m_setup = std::make_unique<TestingSetup>(ChainType::REGTEST, TestOpts{.extra_args={"-keypool=1"}, .setup_net=false});
        m_node = interfaces::MakeNode(m_setup->m_node);
        m_loader = interfaces::MakeWalletLoader(*m_setup->m_node.chain, *m_setup->m_node.args);
        m_setup->m_node.wallet_loader = m_loader.get();
        m_options = std::make_unique<OptionsModel>(*m_node);
        bilingual_str error;
        QVERIFY2(m_options->Init(error), error.original.c_str());
        m_client = std::make_unique<ClientModel>(*m_node, m_options.get());
        m_style.reset(PlatformStyle::instantiate("other"));
    }

    void explicitModeShowsStakeAndPreservesRestrictions()
    {
        auto f = MakeFixture();
        wallet::CCoinControl cc;
        CoinControlDialog dialog(cc, f.model.get(), m_style.get());
        auto* tree = dialog.findChild<QTreeWidget*>("treeWidget");
        auto* include = dialog.findChild<QCheckBox*>("includeStakeOutputs");
        QVERIFY(tree);
        QVERIFY(include); // Fails against the preserved before source: no opt-in exists.
        QVERIFY(!include->isChecked());
        QVERIFY(Find(tree, f.ordinary));
        QVERIFY(!Find(tree, f.pending));
        dialog.findChild<QPushButton*>("pushButtonSelectAll")->click();
        QVERIFY(!cc.IsSelected(f.pending));
        QVERIFY(!cc.IsSelected(f.active));
        cc.UnSelectAll();
        AcceptInclude(include);
        QVERIFY(include->isChecked());
        QVERIFY(Find(tree, f.pending));
        QVERIFY(Find(tree, f.active));
        QVERIFY(Find(tree, f.pending)->text(6).contains("pending"));
        QVERIFY(Find(tree, f.active)->text(6).contains("active by depth"));
        QVERIFY(!Find(tree, f.pending)->isDisabled()); // Activation is not spend maturity.
        QVERIFY(Find(tree, f.locked)->isDisabled());
        QVERIFY(Find(tree, f.immature)->isDisabled());
        Find(tree, f.pending)->setCheckState(0, Qt::Checked);
        QVERIFY(cc.IsSelected(f.pending));
        const COutPoint missing{Txid::FromUint256(uint256::ONE), 999};
        cc.Select(missing);
        CoinControlDialog::updateLabels(cc, f.model.get(), &dialog);
        QVERIFY(cc.IsSelected(missing)); // Unknown STAKE-mode selections require explicit review, not silent fallback.
        QVERIFY(cc.IsSelected(f.pending)); // Missing entries must not shift the outpoint/value pairing.
        cc.UnSelect(missing); // Deliberate owner deselection.
        include->click();
        QVERIFY(!cc.IsSelected(f.pending));
        QVERIFY(!Find(tree, f.pending));
        QVERIFY(f.model->wallet().isLockedCoin(f.locked));
        dialog.reject(); // Selection/cancellation creates no wallet transaction.
        QCOMPARE(f.model->wallet().getWalletTxs().size(), size_t{5});
    }

    void watchOnlyVisibleNeverSelectedAndOtherWalletNotAttributed()
    {
        auto first = MakeFixture(true);
        auto second = MakeFixture();
        wallet::CCoinControl first_cc, second_cc;
        CoinControlDialog first_dialog(first_cc, first.model.get(), m_style.get());
        auto* include = first_dialog.findChild<QCheckBox*>("includeStakeOutputs");
        QVERIFY(include);
        AcceptInclude(include);
        auto* tree = first_dialog.findChild<QTreeWidget*>("treeWidget");
        auto* active = Find(tree, first.active);
        QVERIFY(active);
        QVERIFY(active->isDisabled());
        QVERIFY(active->text(6).contains("Owner spending keys unavailable"));
        QVERIFY(!Find(tree, second.active));
        first_dialog.findChild<QPushButton*>("pushButtonSelectAll")->click();
        QVERIFY(!first_cc.IsSelected(first.active));
        CoinControlDialog second_dialog(second_cc, second.model.get(), m_style.get());
        QVERIFY(!second_dialog.findChild<QCheckBox*>("includeStakeOutputs")->isChecked());
        QVERIFY(second_cc.ListSelected().empty());
    }

    void includeWarningNoAndEscapeDoNotConsent()
    {
        auto f = MakeFixture();
        wallet::CCoinControl cc;
        CoinControlDialog dialog(cc, f.model.get(), m_style.get());
        auto* include = dialog.findChild<QCheckBox*>("includeStakeOutputs");
        auto* tree = dialog.findChild<QTreeWidget*>("treeWidget");
        for (const bool escape : {false, true}) {
            include->click();
            auto* warning = dialog.findChild<QMessageBox*>();
            QVERIFY(warning);
            QVERIFY(warning->defaultButton() == warning->button(QMessageBox::No));
            QVERIFY(!cc.m_include_stake);
            QVERIFY(!include->isEnabled());
            if (escape) QTest::keyClick(warning, Qt::Key_Escape);
            else warning->button(QMessageBox::No)->click();
            QTRY_VERIFY(include->isEnabled());
            QVERIFY(!include->isChecked());
            QVERIFY(!cc.m_include_stake);
            QVERIFY(!Find(tree, f.active));
            QVERIFY(cc.ListSelected().empty());
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        QCOMPARE(f.model->wallet().getWalletTxs().size(), size_t{5});
    }

    void encryptedRelockAndRefreshPreserveExplicitSelection()
    {
        auto f = MakeFixture();
        {
            LOCK(f.wallet->cs_wallet);
            QVERIFY(f.wallet->EncryptWallet("generated-test-passphrase"));
            QVERIFY(f.wallet->Unlock("generated-test-passphrase"));
        }
        SendCoinsDialog send(m_style.get());
        send.setModel(f.model.get());
        QVERIFY(QMetaObject::invokeMethod(&send, "coinControlButtonClicked", Qt::DirectConnection));
        auto* dialog = send.findChild<CoinControlDialog*>();
        QVERIFY(dialog);
        auto* include = dialog->findChild<QCheckBox*>("includeStakeOutputs");
        auto* tree = dialog->findChild<QTreeWidget*>("treeWidget");
        AcceptInclude(include);
        Find(tree, f.active)->setCheckState(0, Qt::Checked);
        auto* cc = send.getCoinControl();
        QVERIFY(cc->IsSelected(f.active));
        {
            LOCK(f.wallet->cs_wallet);
            QVERIFY(f.wallet->Lock());
        }
        CoinControlDialog::updateLabels(*cc, f.model.get(), dialog);
        QVERIFY(cc->IsSelected(f.active));
        QVERIFY(dialog->findChild<QLabel*>("labelCoinControlQuantity")->text().contains("review selection"));
        // A view rebuild is separately tested: programmatic checked/disabled
        // rendering must not run the user-deselection path.
        RebuildViewByChangingMode(*dialog);
        QVERIFY(cc->IsSelected(f.active));
        QVERIFY(Find(tree, f.active)->isDisabled());
        QCOMPARE(Find(tree, f.active)->checkState(0), Qt::Checked);
        QVERIFY(Find(tree, f.active)->text(6).contains("Unlock the owner wallet"));
        {
            LOCK(f.wallet->cs_wallet);
            QVERIFY(f.wallet->Unlock("generated-test-passphrase"));
        }
        RebuildViewByChangingMode(*dialog);
        QVERIFY(cc->IsSelected(f.active));
        QVERIFY(!Find(tree, f.active)->isDisabled());
        // An external user coin lock also requires review, never fallback.
        f.model->wallet().lockCoin(f.active, true);
        RebuildViewByChangingMode(*dialog);
        QVERIFY(cc->IsSelected(f.active));
        QVERIFY(Find(tree, f.active)->isDisabled());
        QCOMPARE(Find(tree, f.active)->checkState(0), Qt::Checked);
        // The explicit Clear-selection action still works on disabled rows.
        dialog->findChild<QPushButton*>("pushButtonSelectAll")->click();
        QVERIFY(cc->ListSelected().empty());
        QVERIFY(f.model->wallet().isLockedCoin(f.active));
        QCOMPARE(f.model->wallet().getWalletTxs().size(), size_t{5});
    }

    void vanishedStakeSelectionRemainsVisibleUntilExplicitlyCleared()
    {
        auto f = MakeFixture();
        wallet::CCoinControl cc;
        CoinControlDialog dialog(cc, f.model.get(), m_style.get());
        AcceptInclude(dialog.findChild<QCheckBox*>("includeStakeOutputs"));
        auto* tree = dialog.findChild<QTreeWidget*>("treeWidget");
        Find(tree, f.active)->setCheckState(0, Qt::Checked);
        {
            LOCK(f.wallet->cs_wallet);
            CMutableTransaction competing_spend;
            competing_spend.vin.emplace_back(f.active);
            competing_spend.vout.emplace_back(COIN, CScript{});
            QVERIFY(f.wallet->AddToWallet(MakeTransactionRef(competing_spend), wallet::TxStateInMempool{}));
        }
        CoinControlDialog::updateLabels(cc, f.model.get(), &dialog);
        QVERIFY(cc.IsSelected(f.active));
        QVERIFY(dialog.findChild<QLabel*>("labelCoinControlQuantity")->text().contains("unavailable"));
        RebuildViewByChangingMode(dialog);
        auto* missing = Find(tree, f.active);
        QVERIFY(missing);
        QVERIFY(missing->text(6).contains("Selected input unavailable"));
        QCOMPARE(missing->checkState(0), Qt::Checked);
        missing->setCheckState(0, Qt::Unchecked);
        QVERIFY(!cc.IsSelected(f.active));
    }

    void unconfirmedStakeUsesExistingSafeCoinPolicy()
    {
        auto f = MakeFixture();
        COutPoint trusted, incoming, replacing, replaced;
        {
            LOCK(f.wallet->cs_wallet);
            const auto script = f.wallet->GetTXO(f.active)->GetTxOut().scriptPubKey;
            const auto add = [&](uint32_t nonce, bool from_me, const char* marker) {
                CMutableTransaction tx;
                tx.nLockTime = nonce;
                tx.vin.emplace_back(from_me ? f.ordinary : COutPoint{Txid::FromUint256(uint256::ONE), nonce});
                tx.vout.emplace_back(100 * COIN, script);
                const auto ref = MakeTransactionRef(tx);
                auto* wtx = f.wallet->AddToWallet(ref, wallet::TxStateInMempool{});
                assert(wtx);
                if (marker) wtx->mapValue[marker] = uint256::ONE.GetHex();
                return COutPoint{ref->GetHash(), 0};
            };
            trusted = add(20, true, nullptr);
            incoming = add(21, false, nullptr);
            replacing = add(22, true, "replaces_txid");
            replaced = add(23, true, "replaced_by_txid");
        }
        wallet::CCoinControl cc;
        CoinControlDialog dialog(cc, f.model.get(), m_style.get());
        AcceptInclude(dialog.findChild<QCheckBox*>("includeStakeOutputs"));
        auto* tree = dialog.findChild<QTreeWidget*>("treeWidget");
        QVERIFY(Find(tree, trusted));
        QVERIFY(!Find(tree, trusted)->isDisabled());
        for (const auto& outpoint : {incoming, replacing, replaced}) {
            QVERIFY(Find(tree, outpoint));
            QVERIFY(Find(tree, outpoint)->isDisabled());
            QVERIFY(Find(tree, outpoint)->text(6).contains("Untrusted or replaced"));
        }
    }

    void removedWalletDoesNotReceiveSurvivingOptionsCallbacks()
    {
        auto f = MakeFixture();
        SendCoinsDialog send(m_style.get());
        send.setModel(f.model.get());
        f.model.reset();
        // Both the real surviving sender and a queued/direct late invocation
        // are covered. Before the fix refreshBalance dereferences null here.
        Q_EMIT m_options->displayUnitChanged(BitcoinUnit::BTC);
        QVERIFY(QMetaObject::invokeMethod(&send, "refreshBalance", Qt::DirectConnection));
        Q_EMIT m_options->coinControlFeaturesChanged(false);
        QVERIFY(send.getCoinControl()->ListSelected().empty());
        QCOMPARE(WITH_LOCK(f.wallet->cs_wallet, return f.wallet->mapWallet.size()), size_t{5});
    }

    void walletSwitchRebindsOptionsWithoutDuplicateFeeChoices()
    {
        OptionsModel alternate_options(*m_node);
        bilingual_str error;
        QVERIFY(alternate_options.Init(error));
        ClientModel alternate_client(*m_node, &alternate_options);
        auto first = MakeFixture(false, &alternate_client);
        auto second = MakeFixture();
        SendCoinsDialog send(m_style.get());
        send.setModel(first.model.get());
        auto* targets = send.findChild<QComboBox*>("confTargetSelector");
        QVERIFY(targets);
        const int target_count = targets->count();
        QVERIFY(target_count > 0);
        send.setModel(second.model.get());
        QCOMPARE(targets->count(), target_count);
        send.setModel(second.model.get());
        QCOMPARE(targets->count(), target_count);
        send.getCoinControl()->Select(second.ordinary);
        Q_EMIT alternate_options.coinControlFeaturesChanged(false);
        QVERIFY(send.getCoinControl()->IsSelected(second.ordinary));
        Q_EMIT m_options->coinControlFeaturesChanged(false);
        QVERIFY(send.getCoinControl()->ListSelected().empty());
    }

    void openCoinControlIsDestroyedBeforeWalletSwitch()
    {
        auto first = MakeFixture();
        auto second = MakeFixture();
        SendCoinsDialog send(m_style.get());
        send.setModel(first.model.get());
        QVERIFY(QMetaObject::invokeMethod(&send, "coinControlButtonClicked", Qt::DirectConnection));
        QPointer<CoinControlDialog> open = send.findChild<CoinControlDialog*>();
        QVERIFY(open);
        send.setModel(second.model.get());
        QVERIFY(!open);
        QVERIFY(send.getCoinControl()->ListSelected().empty());
        QCOMPARE(first.model->wallet().getWalletTxs().size(), size_t{5});
        QCOMPARE(second.model->wallet().getWalletTxs().size(), size_t{5});
    }

    void walletRemovalWhileIncludeWarningIsOpen()
    {
        auto f = MakeFixture();
        SendCoinsDialog send(m_style.get());
        send.setModel(f.model.get());
        auto* recipient = send.findChild<SendCoinsEntry*>();
        QVERIFY(recipient);
        recipient->setAddress("test-recipient-no-economic-action");
        QVERIFY(!recipient->isClear());
        QVERIFY(QMetaObject::invokeMethod(&send, "coinControlButtonClicked", Qt::DirectConnection));
        QPointer<CoinControlDialog> open = send.findChild<CoinControlDialog*>();
        QVERIFY(open);
        auto* include = open->findChild<QCheckBox*>("includeStakeOutputs");
        QVERIFY(include);
        bool warning_observed{false};
        QTimer::singleShot(0, &send, [&] {
            for (auto* widget : QApplication::topLevelWidgets())
                if (qobject_cast<QMessageBox*>(widget)) warning_observed = true;
            f.model.reset(); // Normal model-destruction signal; no wallet contents are reset.
        });
        include->click();
        QTRY_VERIFY(!open);
        QVERIFY(warning_observed);
        QVERIFY(!open);
        QVERIFY(send.getCoinControl()->ListSelected().empty());
        QVERIFY(recipient->isClear());
        recipient->setAddress("read-only-edit-after-model-removal");
        QVERIFY(!recipient->validate(*m_node));
        QCOMPARE(WITH_LOCK(f.wallet->cs_wallet, return f.wallet->mapWallet.size()), size_t{5});
    }

    void cleanupTestCase()
    {
        m_client.reset();
        m_options.reset();
        if (m_setup) m_setup->m_node.wallet_loader = nullptr;
        m_loader.reset();
        m_node.reset();
        m_setup.reset();
    }
};

QTEST_MAIN(B3StakeCoinControlTests)
#include "b3stakecoincontroltests.moc"

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <interfaces/node.h>
#include <qt/createwalletdialog.h>
#include <util/translation.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {
class SyntheticSigner final : public interfaces::ExternalSigner
{
public:
    std::string getName() override { return "unrelated-device-wallet"; }
};
} // namespace

class B3CreateWalletDialogTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void fixedNameIgnoresTypingAndSignerDiscovery_data()
    {
        QTest::addColumn<bool>("signer_first");
        QTest::newRow("before-signer-discovery") << false;
        QTest::newRow("after-signer-discovery") << true;
    }

    void fixedNameIgnoresTypingAndSignerDiscovery()
    {
        QFETCH(bool, signer_first);
        CreateWalletDialog dialog{nullptr};
        std::vector<std::unique_ptr<interfaces::ExternalSigner>> signers;
        signers.push_back(std::make_unique<SyntheticSigner>());
        if (signer_first) dialog.setSigners(signers);
        dialog.setFixedWalletName(QStringLiteral("closed-test"));
        dialog.setSigners(signers);

        auto* name = dialog.findChild<QLineEdit*>("wallet_name_line_edit");
        auto* signer = dialog.findChild<QCheckBox*>("external_signer_checkbox");
        auto* encrypt = dialog.findChild<QCheckBox*>("encrypt_wallet_checkbox");
        auto* buttons = dialog.findChild<QDialogButtonBox*>("buttonBox");
        QVERIFY(name && signer && encrypt && buttons);
        QVERIFY(name->isReadOnly());
        name->selectAll();
        QTest::keyClicks(name, "test");
        QCOMPARE(name->text(), QStringLiteral("closed-test"));
        QCOMPARE(dialog.walletName(), QStringLiteral("closed-test"));
        QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(!signer->isEnabled());
        QVERIFY(!dialog.isExternalSignerChecked());

        // Encryption changes used to re-enable an attached signer. Neither
        // toggling them nor a repeated discovery may escape the guarded name.
        encrypt->setChecked(true);
        encrypt->setChecked(false);
        dialog.setSigners(signers);
        QVERIFY(!signer->isEnabled());
        QVERIFY(!dialog.isExternalSignerChecked());
        QCOMPARE(dialog.walletName(), QStringLiteral("closed-test"));
    }

    void ordinaryDialogStillAllowsCustomNameAndSigner()
    {
        CreateWalletDialog dialog{nullptr};
        auto* name = dialog.findChild<QLineEdit*>("wallet_name_line_edit");
        auto* buttons = dialog.findChild<QDialogButtonBox*>("buttonBox");
        QVERIFY(name && buttons);
        QVERIFY(!name->isReadOnly());
        QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
        QTest::keyClicks(name, "ordinary-wallet");
        QCOMPARE(dialog.walletName(), QStringLiteral("ordinary-wallet"));
        QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        std::vector<std::unique_ptr<interfaces::ExternalSigner>> signers;
        signers.push_back(std::make_unique<SyntheticSigner>());
        dialog.setSigners(signers);
        QCOMPARE(dialog.walletName(), QStringLiteral("unrelated-device-wallet"));
        QVERIFY(dialog.isExternalSignerChecked());
    }

    void existingEntriesPreventSecondWallet_data()
    {
        QTest::addColumn<QString>("entry");
        QTest::addColumn<bool>("directory");
        QTest::newRow("existing-test-wallet") << QStringLiteral("test") << true;
        QTest::newRow("existing-closed-test-wallet") << QStringLiteral("closed-test") << true;
        QTest::newRow("unrecognized-directory") << QStringLiteral("unrecognized") << true;
        QTest::newRow("hidden-directory") << QStringLiteral(".hidden") << true;
        QTest::newRow("hidden-file") << QStringLiteral(".marker") << false;
        QTest::newRow("ordinary-file") << QStringLiteral("wallet.dat") << false;
    }

    void existingEntriesPreventSecondWallet()
    {
        QFETCH(QString, entry);
        QFETCH(bool, directory);
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString root{QDir{tmp.path()}.filePath("wallets")};
        QVERIFY(!CreateWalletDialog::isEmptyWalletDirectory(root));
        QVERIFY(QDir{}.mkpath(root));
        QVERIFY(CreateWalletDialog::isEmptyWalletDirectory(root));
        const QString path{QDir{root}.filePath(entry)};
        if (directory) {
            QVERIFY(QDir{}.mkpath(path));
        } else {
            QFile file{path};
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("preserve-me"), qint64{11});
            file.close();
        }
        // Covers both the initial inspection and a late entry arriving before
        // the serialized worker rechecks. Inspection must never remove it.
        QVERIFY(!CreateWalletDialog::isEmptyWalletDirectory(root));
        QVERIFY(!CreateWalletDialog::isEmptyWalletDirectory(root));
        QVERIFY(QFileInfo::exists(path));
        if (!directory) {
            QFile file{path};
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), QByteArray{"preserve-me"});
        }
    }

    void fileIsNotAnEmptyWalletDirectory()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QFile file{QDir{tmp.path()}.filePath("not-a-directory")};
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QVERIFY(!CreateWalletDialog::isEmptyWalletDirectory(file.fileName()));
    }
};

QTEST_MAIN(B3CreateWalletDialogTests)
#include "b3createwalletdialogtests.moc"

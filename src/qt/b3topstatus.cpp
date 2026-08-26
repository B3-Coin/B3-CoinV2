// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3topstatus.h>

#include <qt/b3theme.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QStyle>

B3TopStatus::B3TopStatus(QWidget* parent)
    : QFrame{parent}
{
    setObjectName("B3TopStatus");
    setFixedHeight(66);
    // Let B3Shell receive the requested narrow geometry before it switches
    // this strip into compact mode. Otherwise the expanded child labels can
    // clamp a one-step wide-to-narrow resize above the compact threshold.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(28, B3Theme::kSpaceSm, 28,
                                 B3Theme::kSpaceSm);
    m_layout->setSpacing(B3Theme::kSpaceMd);

    m_brand = new QLabel(tr("Overview"), this);
    m_brand->setObjectName("B3TopStatusTitle");
    B3Theme::markTextRole(m_brand, "title");
    m_brand->setAccessibleName(tr("Current section"));

    m_netBadge = new QLabel(this);
    m_netBadge->setObjectName("B3NetBadge");
    m_netBadge->setAccessibleName(tr("Active network"));
    m_netBadge->setAlignment(Qt::AlignCenter);
    m_netBadge->setFixedHeight(32);

    m_layout->addWidget(m_brand);
    m_layout->addStretch(1);

    m_sync = makeChip("statusSync");
    m_sync->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_connections = makeChip("statusConnections");
    m_staking = makeChip("statusStaking");
    m_wallet = makeChip("statusWallet");
    m_staking->setVisible(false);

    m_layout->addWidget(m_netBadge);
    m_layout->addWidget(m_sync);
    m_layout->addWidget(m_connections);
    m_layout->addWidget(m_staking);
    m_layout->addWidget(m_wallet);

    auto* trailingHost = new QWidget(this);
    m_trailing = new QHBoxLayout(trailingHost);
    m_trailing->setContentsMargins(0, 0, 0, 0);
    m_trailing->setSpacing(B3Theme::kSpaceSm);
    m_layout->addWidget(trailingHost);

    setNetwork(tr("B3 Hive"), QString());
    setConnections(0, true);
    setSync(tr("Connecting…"), -1);
    setWalletStatus(QString());
}

void B3TopStatus::setSectionTitle(const QString& title)
{
    m_brand->setText(title);
}

QLabel* B3TopStatus::makeChip(const QString& objectName)
{
    auto* label = new QLabel(this);
    label->setObjectName(objectName);
    label->setProperty("b3status", true);
    return label;
}

void B3TopStatus::setNetwork(const QString& app_name, const QString& title_add)
{
    // title_add is empty on mainnet; non-empty and prominent otherwise so
    // testnet/regtest is unmistakable.
    QString net{title_add.trimmed()};
    net.remove('[').remove(']');
    QString label{tr("MAINNET")};
    QString networkProperty{QStringLiteral("mainnet")};
    if (net.contains("regtest", Qt::CaseInsensitive)) {
        label = tr("REGTEST");
        networkProperty = QStringLiteral("regtest");
    } else if (!net.isEmpty()) {
        label = net.toUpper();
        networkProperty = QStringLiteral("testnet");
    }
    m_netBadge->setText(label);
    m_netBadge->setProperty("b3network", networkProperty);
    m_netBadge->style()->unpolish(m_netBadge);
    m_netBadge->style()->polish(m_netBadge);
    m_netBadge->setAccessibleDescription(tr("Connected to %1 (%2)").arg(app_name, label));
    m_netBadge->setToolTip(app_name);
}

void B3TopStatus::setConnections(int count, bool network_active)
{
    if (!network_active) {
        m_connections->setText(tr("Network off"));
    } else {
        m_connections->setText(tr("%n peer(s)", "", count));
    }
    m_connections->setAccessibleName(tr("Connection status"));
}

void B3TopStatus::setSync(const QString& text, int permille)
{
    if (permille >= 0 && permille < 1000) {
        m_sync->setText(tr("Syncing %1% — %2").arg(permille / 10.0, 0, 'f', 1).arg(text));
    } else {
        m_sync->setText(text);
    }
    m_sync->setToolTip(m_sync->text());
    m_sync->setAccessibleName(tr("Synchronization status"));
}

void B3TopStatus::setWalletStatus(const QString& text)
{
    m_wallet->setText(text);
    m_wallet->setVisible(!m_compact && !text.isEmpty());
    m_wallet->setAccessibleName(tr("Wallet status"));
}

void B3TopStatus::setStakingStatus(const QString& text)
{
    m_staking->setText(text);
    m_staking->setVisible(!m_compact && !text.isEmpty());
    m_staking->setAccessibleName(tr("Staking status"));
}

void B3TopStatus::addTrailingWidget(QWidget* widget)
{
    if (widget) m_trailing->addWidget(widget);
}

void B3TopStatus::setCompact(bool compact)
{
    if (m_compact == compact) return;
    m_compact = compact;
    m_layout->setContentsMargins(28,
                                 B3Theme::kSpaceSm,
                                 28,
                                 B3Theme::kSpaceSm);
    m_layout->setSpacing(compact ? B3Theme::kSpaceSm : B3Theme::kSpaceMd);
    m_brand->setVisible(true);
    m_sync->setVisible(!compact);
    m_connections->setVisible(!compact);
    m_wallet->setVisible(!compact && !m_wallet->text().isEmpty());
    m_staking->setVisible(!compact && !m_staking->text().isEmpty());
}

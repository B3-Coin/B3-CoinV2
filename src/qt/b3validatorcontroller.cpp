// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3validatorcontroller.h>

#include <addresstype.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <qt/walletmodel.h>
#include <rpc/util.h>
#include <univalue.h>
#include <util/moneystr.h>
#include <util/result.h>

#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <algorithm>
#include <exception>
#include <utility>

namespace {

constexpr int STATUS_REFRESH_MS{5'000};
constexpr int SUCCESSFUL_BLOCK_SPACING_MS{60'000};
constexpr int FAILED_ATTEMPT_RETRY_MS{5'000};
constexpr uint64_t CORRIDOR_MAX_TRIES{1'000'000};

QString RpcErrorText(const UniValue& error)
{
    try {
        const UniValue& message{error.find_value("message")};
        if (message.isStr()) return QString::fromStdString(message.get_str());
    } catch (const std::exception&) {
    }
    return QString::fromStdString(error.write());
}

QString StringField(const UniValue& object, const char* key)
{
    const UniValue& value{object.find_value(key)};
    return value.isStr() ? QString::fromStdString(value.get_str()) : QString{};
}

bool BoolField(const UniValue& object, const char* key, const bool fallback = false)
{
    const UniValue& value{object.find_value(key)};
    return value.isBool() ? value.get_bool() : fallback;
}

template <typename T>
T IntField(const UniValue& object, const char* key, const T fallback = T{})
{
    const UniValue& value{object.find_value(key)};
    return value.isNum() ? value.getInt<T>() : fallback;
}

CAmount AmountField(const UniValue& object, const char* key)
{
    const UniValue& value{object.find_value(key)};
    return value.isNull() ? 0 : AmountFromValue(value);
}

QVariant ToVariant(const UniValue& value)
{
    if (value.isNull()) return {};
    if (value.isBool()) return value.get_bool();
    if (value.isStr()) return QString::fromStdString(value.get_str());
    // Keep JSON numbers as their exact lexical representation. In particular,
    // a nine-decimal B3 amount must never pass through double in the GUI.
    if (value.isNum()) return QString::fromStdString(value.getValStr());
    if (value.isArray()) {
        QVariantList result;
        result.reserve(static_cast<qsizetype>(value.size()));
        for (const UniValue& item : value.getValues()) result.push_back(ToVariant(item));
        return result;
    }
    QVariantMap result;
    const auto& keys{value.getKeys()};
    const auto& values{value.getValues()};
    for (size_t i{0}; i < keys.size(); ++i) {
        result.insert(QString::fromStdString(keys[i]), ToVariant(values[i]));
    }
    return result;
}

QVariantMap ToVariantMap(const UniValue& value)
{
    return value.isObject() ? ToVariant(value).toMap() : QVariantMap{};
}

B3ValidatorStatus ParseStatus(const UniValue& staking, const UniValue* finality)
{
    if (!staking.isObject()) throw std::runtime_error{"getstakinginfo returned a non-object"};

    B3ValidatorStatus status;
    status.valid = true;
    status.tip_height = IntField<int>(staking, "tip_height", -1);
    status.next_block_phase = StringField(staking, "next_block_phase");
    status.modern_pos_active = BoolField(staking, "modern_pos_active");
    const UniValue& minimum{staking.find_value("min_stake_amount")};
    if (!minimum.isNull()) {
        status.min_stake_available = true;
        status.min_stake_amount = AmountFromValue(minimum);
    }
    status.activation_depth = IntField<int>(staking, "activation_depth");
    status.validator_key = StringField(staking, "validator_key");
    status.eligible_weight = IntField<uint64_t>(staking, "active_weight");
    status.total_eligible_weight = IntField<uint64_t>(staking, "total_active_weight");
    status.active_stake = AmountField(staking, "active");
    status.pending_stake = AmountField(staking, "pending");
    status.unconfirmed_stake = AmountField(staking, "unconfirmed");

    const UniValue& loop{staking.find_value("staking")};
    if (loop.isObject()) {
        status.staking_available = BoolField(loop, "available");
        status.staking_running = BoolField(loop, "running");
        status.staking_validator_key = StringField(loop, "validator_key");
        status.staking_uses_this_wallet = status.staking_running &&
            !status.validator_key.isEmpty() &&
            status.staking_validator_key == status.validator_key;
        status.staking_state = StringField(loop, "state");
        status.staking_last_error = StringField(loop, "last_error");
        status.finality_signing = BoolField(loop, "finality_signing");
        status.last_signed_height = IntField<int>(loop, "last_signed_height", -1);
    }

    if (finality && finality->isObject()) {
        const UniValue& binding{finality->find_value("binding")};
        if (binding.isObject()) {
            status.finality_bound = BoolField(binding, "bound");
            status.finality_revoked = BoolField(binding, "revoked");
            status.bls_pubkey = StringField(binding, "bls_pubkey");
        }
        const UniValue& validator_set{finality->find_value("validator_set")};
        if (validator_set.isObject()) {
            status.current_set_member = BoolField(validator_set, "member");
            status.member_weight = IntField<uint64_t>(validator_set, "weight");
            status.total_weight = IntField<uint64_t>(validator_set, "total_weight");
            status.quorum_weight = IntField<uint64_t>(validator_set, "quorum_weight");
        }
        const UniValue& signing{finality->find_value("signing")};
        if (signing.isObject()) {
            status.finality_signing = BoolField(signing, "armed");
            status.last_signed_height = IntField<int>(signing, "last_signed_height", -1);
        }
    }
    return status;
}

std::string WalletUri(const QString& wallet_name)
{
    if (wallet_name.isEmpty()) return {};
    const QByteArray encoded{QUrl::toPercentEncoding(wallet_name)};
    return "/wallet/" + std::string{encoded.constData(), static_cast<size_t>(encoded.size())};
}

} // namespace

B3ValidatorController::B3ValidatorController(QObject* parent)
    : QObject{parent},
      m_worker_thread{new QThread{this}},
      m_worker{new QObject},
      m_refresh_timer{new QTimer{this}},
      m_mining_timer{new QTimer{this}}
{
    qRegisterMetaType<B3ValidatorStatus>("B3ValidatorStatus");

    m_worker->moveToThread(m_worker_thread);
    connect(m_worker_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_worker_thread->start();

    m_refresh_timer->setInterval(STATUS_REFRESH_MS);
    connect(m_refresh_timer, &QTimer::timeout, this, &B3ValidatorController::refresh);

    m_mining_timer->setSingleShot(true);
    connect(m_mining_timer, &QTimer::timeout, this, &B3ValidatorController::queueMiningAttempt);
}

B3ValidatorController::~B3ValidatorController()
{
    m_refresh_timer->stop();
    m_mining_timer->stop();
    m_auto_mining = false;
    ++m_generation;
    m_worker_thread->quit();
    m_worker_thread->wait();
}

void B3ValidatorController::setWalletModel(WalletModel* wallet_model)
{
    if (m_wallet_model == wallet_model) {
        refresh();
        return;
    }

    if (m_wallet_model) disconnect(m_wallet_model, nullptr, this, nullptr);
    if (m_auto_mining) Q_EMIT autoCorridorMiningChanged(false);
    m_auto_mining = false;
    m_refresh_timer->stop();
    m_mining_timer->stop();
    m_refresh_in_flight = false;
    m_mining_in_flight = false;
    m_mining_address.clear();
    m_mining_error.clear();
    m_pending_validator_key.clear();
    m_pending_bls_pubkey.clear();
    ++m_generation;

    m_wallet_model = wallet_model;
    m_status = {};
    publishStatus();

    if (!m_wallet_model) return;
    connect(m_wallet_model, &QObject::destroyed, this, [this] { setWalletModel(nullptr); });
    m_refresh_timer->start();
    refresh();
}

std::string B3ValidatorController::walletUri() const
{
    return m_wallet_model ? WalletUri(m_wallet_model->getWalletName()) : std::string{};
}

void B3ValidatorController::refresh()
{
    if (!m_wallet_model || m_refresh_in_flight) return;

    interfaces::Node* const node{&m_wallet_model->node()};
    const std::string uri{walletUri()};
    const uint64_t generation{m_generation};
    QPointer<B3ValidatorController> self{this};
    m_refresh_in_flight = true;

    QMetaObject::invokeMethod(m_worker, [self, node, uri, generation] {
        B3ValidatorStatus status;
        QString error;
        try {
            const UniValue empty{UniValue::VARR};
            const UniValue staking{node->executeRpc("getstakinginfo", empty, uri)};
            UniValue finality;
            const QString validator{StringField(staking, "validator_key")};
            if (!validator.isEmpty()) {
                try {
                    finality = node->executeRpc("getfinalityinfo", empty, uri);
                } catch (UniValue& rpc_error) {
                    error = RpcErrorText(rpc_error);
                } catch (const std::exception& e) {
                    error = QString::fromStdString(e.what());
                }
            }
            status = ParseStatus(staking, finality.isObject() ? &finality : nullptr);
            status.refresh_error = error;
        } catch (UniValue& rpc_error) {
            error = RpcErrorText(rpc_error);
        } catch (const std::exception& e) {
            error = QString::fromStdString(e.what());
        }

        if (!self) return;
        QMetaObject::invokeMethod(self, [self, generation, status = std::move(status), error] () mutable {
            if (self) self->applyRefresh(generation, std::move(status), error);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void B3ValidatorController::applyRefresh(const uint64_t generation, B3ValidatorStatus status,
                                         const QString& error)
{
    if (generation != m_generation || !m_wallet_model) return;
    m_refresh_in_flight = false;
    if (!error.isEmpty() && !status.valid) {
        status.refresh_error = error;
    }

    if (!m_pending_validator_key.isEmpty()) {
        if (status.validator_key.isEmpty()) status.validator_key = m_pending_validator_key;
        if (status.validator_key == m_pending_validator_key &&
            status.bls_pubkey != m_pending_bls_pubkey) {
            status.bls_pubkey = m_pending_bls_pubkey;
            status.binding_pending = true;
        } else if (status.finality_bound && status.bls_pubkey == m_pending_bls_pubkey) {
            m_pending_validator_key.clear();
            m_pending_bls_pubkey.clear();
        }
    }

    m_status = std::move(status);
    publishStatus();
}

void B3ValidatorController::executeUnlocked(const QString& operation,
                                            const std::string& method,
                                            const UniValue& params)
{
    if (!m_wallet_model) {
        Q_EMIT operationFailed(operation, tr("No wallet is loaded."));
        return;
    }

    QVariantMap details;
    QString failure;
    bool succeeded{false};
    {
        // Relock before any result signal reaches the page. In particular, an
        // error signal can open a modal dialog and must never extend the
        // wallet's unlocked lifetime while the user reads it.
        WalletModel::UnlockContext unlock{m_wallet_model->requestUnlock()};
        if (!unlock.isValid()) {
            failure = tr("The wallet was not unlocked.");
        } else {
            try {
                const UniValue result{m_wallet_model->node().executeRpc(method, params, walletUri())};
                details = ToVariantMap(result);
                if (operation == QStringLiteral("bind_finality_key")) {
                    m_pending_validator_key = details.value(QStringLiteral("validator_key")).toString();
                    m_pending_bls_pubkey = details.value(QStringLiteral("bls_pubkey")).toString();
                }
                succeeded = true;
            } catch (UniValue& rpc_error) {
                failure = RpcErrorText(rpc_error);
            } catch (const std::exception& e) {
                failure = QString::fromStdString(e.what());
            }
        }
    }

    if (succeeded) {
        Q_EMIT operationSucceeded(operation, details);
        refresh();
    } else {
        Q_EMIT operationFailed(operation,
                               failure.isEmpty() ? tr("The wallet operation failed.") : failure);
    }
}

void B3ValidatorController::bindFinalityKey()
{
    executeUnlocked(QStringLiteral("bind_finality_key"), "bindfinalitykey",
                    UniValue{UniValue::VARR});
}

void B3ValidatorController::createStake(const CAmount amount)
{
    if (amount <= 0) {
        Q_EMIT operationFailed(QStringLiteral("create_stake"), tr("Enter a positive stake amount."));
        return;
    }
    UniValue amount_value;
    amount_value.setNumStr(FormatMoney(amount));
    UniValue params{UniValue::VARR};
    params.push_back(std::move(amount_value));
    executeUnlocked(QStringLiteral("create_stake"), "createstake", params);
}

void B3ValidatorController::startStaking()
{
    const QString operation{QStringLiteral("start_staking")};
    if (!m_wallet_model) {
        Q_EMIT operationFailed(operation, tr("No wallet is loaded."));
        return;
    }
    if (m_status.staking_running) {
        Q_EMIT operationFailed(
            operation,
            m_status.staking_uses_this_wallet
                ? tr("Staking is already running with this wallet.")
                : tr("This node is already staking with another wallet's validator."));
        return;
    }

    QVariantMap details;
    QString failure;
    bool succeeded{false};
    {
        WalletModel::UnlockContext unlock{m_wallet_model->requestUnlock()};
        if (!unlock.isValid()) {
            failure = tr("The wallet was not unlocked.");
        } else {
            try {
                // The staking loop is node-global. Only start after proving
                // that this unlocked wallet owns its live binding.
                const UniValue empty{UniValue::VARR};
                const UniValue finality{m_wallet_model->node().executeRpc(
                    "getfinalityinfo", empty, walletUri())};
                const UniValue& binding{finality.find_value("binding")};
                if (!binding.isObject() || !BoolField(binding, "bound") ||
                    BoolField(binding, "revoked")) {
                    failure = tr("Confirm this wallet's BLS finality binding before starting staking.");
                } else if (!BoolField(binding, "key_is_ours")) {
                    failure = tr("This wallet does not hold the BLS key bound to its validator.");
                } else {
                    const UniValue result{m_wallet_model->node().executeRpc(
                        "startstaking", empty, walletUri())};
                    details = ToVariantMap(result);
                    if (!details.value(QStringLiteral("finality_signing")).toBool()) {
                        // Defensive fail closed: never leave a producer
                        // running without its exact finality signer.
                        m_wallet_model->node().executeRpc("stopstaking", empty, walletUri());
                        failure = tr("Staking was stopped because the finality signer was not armed.");
                    } else {
                        succeeded = true;
                    }
                }
            } catch (UniValue& rpc_error) {
                failure = RpcErrorText(rpc_error);
            } catch (const std::exception& e) {
                failure = QString::fromStdString(e.what());
            }
        }
    }

    // UnlockContext has relocked an originally locked wallet before the page
    // can display either the success state or a modal error.
    if (succeeded) {
        Q_EMIT operationSucceeded(operation, details);
        refresh();
    } else {
        Q_EMIT operationFailed(operation,
                               failure.isEmpty() ? tr("The validator did not start.") : failure);
        refresh();
    }
}

void B3ValidatorController::stopStaking()
{
    const QString operation{QStringLiteral("stop_staking")};
    if (!m_wallet_model) {
        Q_EMIT operationFailed(operation, tr("No wallet is loaded."));
        return;
    }
    if (m_status.staking_running && !m_status.staking_uses_this_wallet) {
        Q_EMIT operationFailed(operation,
            tr("Another wallet owns the node's active staking loop; switch to that wallet to stop it."));
        return;
    }
    try {
        const UniValue result{m_wallet_model->node().executeRpc(
            "stopstaking", UniValue{UniValue::VARR}, walletUri())};
        Q_EMIT operationSucceeded(operation, ToVariantMap(result));
        refresh();
    } catch (UniValue& rpc_error) {
        Q_EMIT operationFailed(operation, RpcErrorText(rpc_error));
    } catch (const std::exception& e) {
        Q_EMIT operationFailed(operation, QString::fromStdString(e.what()));
    }
}

void B3ValidatorController::startAutoCorridorMining()
{
    const QString operation{QStringLiteral("start_corridor_mining")};
    if (!m_wallet_model) {
        Q_EMIT operationFailed(operation, tr("No wallet is loaded."));
        return;
    }
    if (m_auto_mining) return;
    if (m_mining_in_flight) {
        Q_EMIT operationFailed(operation, tr("The previous bounded mining attempt is still finishing."));
        return;
    }
    if (!m_status.valid || m_status.next_block_phase != QStringLiteral("corridor")) {
        Q_EMIT operationFailed(operation, tr("The next block is not a transition-corridor block."));
        return;
    }

    // Mainnet intentionally keeps SegWit disabled, so the corridor payout must
    // always use the standard B3 P2PKH form rather than a wallet preference.
    const auto destination{m_wallet_model->wallet().getNewDestination(
        OutputType::LEGACY, "b3-corridor-mining")};
    if (!destination) {
        Q_EMIT operationFailed(operation,
            QString::fromStdString(util::ErrorString(destination).original));
        return;
    }

    m_mining_address = QString::fromStdString(EncodeDestination(*destination));
    m_mining_error.clear();
    m_auto_mining = true;
    Q_EMIT autoCorridorMiningChanged(true);
    QVariantMap details;
    details.insert(QStringLiteral("address"), m_mining_address);
    Q_EMIT operationSucceeded(operation, details);
    publishStatus();
    queueMiningAttempt();
}

void B3ValidatorController::stopAutoCorridorMining()
{
    const bool was_active{m_auto_mining};
    m_auto_mining = false;
    m_mining_timer->stop();
    m_mining_error.clear();
    if (was_active) {
        Q_EMIT autoCorridorMiningChanged(false);
        QVariantMap details;
        details.insert(QStringLiteral("address"), m_mining_address);
        Q_EMIT operationSucceeded(QStringLiteral("stop_corridor_mining"), details);
        publishStatus();
    }
}

void B3ValidatorController::queueMiningAttempt()
{
    if (!m_auto_mining || m_mining_in_flight || !m_wallet_model) return;

    interfaces::Node* const node{&m_wallet_model->node()};
    const std::string uri{walletUri()};
    const std::string address{m_mining_address.toStdString()};
    const uint64_t generation{m_generation};
    QPointer<B3ValidatorController> self{this};
    m_mining_in_flight = true;
    publishStatus();

    QMetaObject::invokeMethod(m_worker, [self, node, uri, address, generation] {
        QString phase;
        QString block_hash;
        QString error;
        try {
            const UniValue empty{UniValue::VARR};
            const UniValue staking{node->executeRpc("getstakinginfo", empty, uri)};
            phase = StringField(staking, "next_block_phase");
            if (phase == QStringLiteral("corridor")) {
                UniValue params{UniValue::VARR};
                params.push_back(UniValue{1});
                params.push_back(UniValue{address});
                params.push_back(UniValue{CORRIDOR_MAX_TRIES});
                const UniValue result{node->executeRpc("generatetoaddress", params, uri)};
                if (result.isArray() && !result.empty() &&
                    result[static_cast<size_t>(0)].isStr()) {
                    block_hash = QString::fromStdString(
                        result[static_cast<size_t>(0)].get_str());
                } else {
                    error = QObject::tr("No block was found in 1,000,000 tries; retrying.");
                }
            }
        } catch (UniValue& rpc_error) {
            error = RpcErrorText(rpc_error);
        } catch (const std::exception& e) {
            error = QString::fromStdString(e.what());
        }

        if (!self) return;
        QMetaObject::invokeMethod(self, [self, generation, phase, block_hash, error] {
            if (self) self->applyMiningResult(generation, phase, block_hash, error);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void B3ValidatorController::applyMiningResult(const uint64_t generation,
                                              const QString& phase,
                                              const QString& block_hash,
                                              const QString& error)
{
    if (generation != m_generation || !m_wallet_model) return;
    m_mining_in_flight = false;

    // A failed phase probe is not evidence that the corridor ended. Keep the
    // operator's opt-in active and retry after a short delay; only a verified
    // non-corridor phase may stop mining as a successful phase transition.
    if (phase.isEmpty() && !error.isEmpty()) {
        m_mining_error = error;
        if (m_auto_mining) m_mining_timer->start(FAILED_ATTEMPT_RETRY_MS);
        publishStatus();
        Q_EMIT operationFailed(QStringLiteral("mine_corridor_block"), error);
        return;
    }

    if (phase != QStringLiteral("corridor")) {
        const bool was_active{m_auto_mining};
        m_auto_mining = false;
        m_mining_timer->stop();
        if (was_active) Q_EMIT autoCorridorMiningChanged(false);
        QVariantMap details;
        details.insert(QStringLiteral("reason"), QStringLiteral("phase_changed"));
        details.insert(QStringLiteral("phase"), phase);
        Q_EMIT operationSucceeded(QStringLiteral("stop_corridor_mining"), details);
        publishStatus();
        refresh();
        return;
    }

    if (!block_hash.isEmpty()) {
        m_mining_error.clear();
        QVariantMap details;
        details.insert(QStringLiteral("block_hash"), block_hash);
        details.insert(QStringLiteral("address"), m_mining_address);
        refresh();
        // This delay starts only after acceptance, guaranteeing at least sixty
        // seconds between successful local attempts.
        if (m_auto_mining) m_mining_timer->start(SUCCESSFUL_BLOCK_SPACING_MS);
        publishStatus();
        Q_EMIT operationSucceeded(QStringLiteral("mine_corridor_block"), details);
    } else {
        m_mining_error = error.isEmpty() ? tr("The mining attempt failed.") : error;
        if (m_auto_mining) m_mining_timer->start(FAILED_ATTEMPT_RETRY_MS);
        publishStatus();
        Q_EMIT operationFailed(QStringLiteral("mine_corridor_block"), m_mining_error);
    }
}

void B3ValidatorController::publishStatus()
{
    m_status.auto_corridor_mining = m_auto_mining;
    m_status.corridor_mining_attempt = m_mining_in_flight;
    m_status.corridor_payout_address = m_mining_address;
    m_status.corridor_mining_error = m_mining_error;
    Q_EMIT statusChanged(m_status);
}

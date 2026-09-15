// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef B3_QT_TEST_FLOWMESHCLOSEDTEST_POLICY_H
#define B3_QT_TEST_FLOWMESHCLOSEDTEST_POLICY_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace FlowMeshClosedTest {
struct Profile {
    bool ready{false};
    QString id;
    QString reason;
    QString peer;
    QStringList endpoints;
    QByteArray ca;
    QByteArray hash;
};

// Only the embedded build-time profile is accepted. No runtime profile or node
// argument forwarding exists in the portable launcher.
bool ParseProfile(const QByteArray& json, Profile& result, QString& error);
bool CheckInvocation(int argc, QString& error);

struct Storage {
    QString root;
    QString node;
    QString wallets;
    QString settings;
    QString ca;
    Storage() = default;
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    ~Storage();
private:
    int lock_fd{-1};
    friend bool PrepareStorage(const Profile&, const QString&, Storage&, QString&);
};

// Called only after a ready profile passes validation. The base must be the
// platform's GenericDataLocation (or a generated test directory in pure tests).
// This guards accidental reuse/path redirection, not a hostile local account.
bool PrepareStorage(const Profile&, const QString& base, Storage&, QString& error);
QStringList NodeArguments(const Profile&, const Storage&);
} // namespace FlowMeshClosedTest
#endif

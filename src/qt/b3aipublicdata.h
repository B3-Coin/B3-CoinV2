// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#ifndef B3_QT_B3AIPUBLICDATA_H
#define B3_QT_B3AIPUBLICDATA_H

#include <QJsonValue>
#include <QString>

namespace B3AiPublicData {
// Project known RPC schemas onto the public observations needed by the model.
// Unknown fields (including future RPC additions) are never forwarded.
QJsonValue SanitizeResult(const QString& tool, const QJsonValue& raw);
}

#endif // B3_QT_B3AIPUBLICDATA_H

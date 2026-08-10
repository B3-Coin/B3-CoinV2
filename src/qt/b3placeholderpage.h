// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3PLACEHOLDERPAGE_H
#define BITCOIN_QT_B3PLACEHOLDERPAGE_H

#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
QT_END_NAMESPACE

/**
 * A calm, honest empty-state page: a title, an explanatory line, and an
 * optional status note. Used wherever a feature has no backend yet, so the
 * UI never fabricates data — it says plainly what is and is not available.
 */
class B3PlaceholderPage : public QWidget
{
    Q_OBJECT

public:
    B3PlaceholderPage(const QString& title, const QString& body, QWidget* parent = nullptr);

    void setBody(const QString& body);
    void setNote(const QString& note);

private:
    QLabel* m_body{nullptr};
    QLabel* m_note{nullptr};
};

#endif // BITCOIN_QT_B3PLACEHOLDERPAGE_H

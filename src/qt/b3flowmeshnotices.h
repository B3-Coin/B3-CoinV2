// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHNOTICES_H
#define BITCOIN_QT_B3FLOWMESHNOTICES_H

#include <QPointer>
#include <QStringList>
#include <QWidget>

#include <chrono>
#include <cstddef>
#include <deque>
#include <set>

class QFrame;
class QVBoxLayout;

/** A strip of short, dismissible notices for the trading panel.
 *
 * Display only: a toast never enables, approves or resends anything. Each
 * key is shown at most once for the widget's lifetime (the key set is
 * FIFO-bounded), at most three toasts are on screen (the oldest goes
 * first), and each disappears after its own interval or when dismissed. */
class B3FlowMeshToasts : public QWidget
{
    Q_OBJECT
public:
    enum class Kind { Info, Success, Warning, Error };
    static constexpr int MAX_VISIBLE{3};
    static constexpr size_t MAX_KEYS{1024};

    explicit B3FlowMeshToasts(QWidget* parent = nullptr);
    ~B3FlowMeshToasts() override;
    //! Shows a toast unless its key was already shown. Returns whether it was.
    bool push(const QString& key, Kind kind, const QString& text, std::chrono::milliseconds ttl);
    //! Removes every toast and forgets every key (a wallet switch).
    void clear();
    //! The texts on screen, oldest first.
    QStringList texts() const;

private:
    void prune(const QObject* removed = nullptr);
    QVBoxLayout* m_layout{nullptr};
    std::deque<QPointer<QFrame>> m_frames;
    std::set<QString> m_keys;
    std::deque<QString> m_key_order;
};

#endif // BITCOIN_QT_B3FLOWMESHNOTICES_H

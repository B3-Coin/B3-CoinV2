// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshnotices.h>

#include <qt/b3theme.h>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

B3FlowMeshToasts::B3FlowMeshToasts(QWidget* parent) : QWidget{parent}
{
    setObjectName(QStringLiteral("flowMeshToasts"));
    setAccessibleName(tr("Trading notices"));
    m_layout = new QVBoxLayout{this};
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(4);
    hide();
}

bool B3FlowMeshToasts::push(const QString& key, const Kind kind, const QString& text, const std::chrono::milliseconds ttl)
{
    if (key.isEmpty() || text.isEmpty() || !m_keys.insert(key).second) return false;
    m_key_order.push_back(key);
    while (m_key_order.size() > MAX_KEYS) {
        m_keys.erase(m_key_order.front());
        m_key_order.pop_front();
    }
    prune();
    while (static_cast<int>(m_frames.size()) >= MAX_VISIBLE) {
        // Out of the list before deleting: its destruction prunes the list.
        const QPointer<QFrame> oldest{m_frames.front()};
        m_frames.pop_front();
        if (oldest) delete oldest.data();
    }
    auto* frame{new QFrame{this}};
    frame->setObjectName(QStringLiteral("flowMeshToast"));
    B3Theme::markCard(frame);
    auto* row{new QHBoxLayout{frame}};
    row->setContentsMargins(10, 6, 6, 6);
    auto* label{new QLabel{text, frame}};
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    B3Theme::markTextRole(label, kind == Kind::Success ? QStringLiteral("positive") : kind == Kind::Warning ? QStringLiteral("accent") :
                                 kind == Kind::Error ? QStringLiteral("negative") : QStringLiteral("secondary"));
    row->addWidget(label, 1);
    auto* dismiss{new QToolButton{frame}};
    dismiss->setText(QStringLiteral("×"));
    dismiss->setAutoRaise(true);
    dismiss->setAccessibleName(tr("Dismiss notice"));
    row->addWidget(dismiss);
    connect(dismiss, &QToolButton::clicked, frame, &QObject::deleteLater);
    // The frame owns its expiry: a deleted toast never fires a late timer.
    QTimer::singleShot(ttl, frame, &QObject::deleteLater);
    connect(frame, &QObject::destroyed, this, [this](QObject* removed) { prune(removed); });
    m_layout->addWidget(frame);
    m_frames.emplace_back(frame);
    show();
    return true;
}

B3FlowMeshToasts::~B3FlowMeshToasts()
{
    // QWidget deletes its children after this object's members are gone:
    // their destruction must not reach prune().
    for (auto* frame : findChildren<QFrame*>(QString{}, Qt::FindDirectChildrenOnly)) disconnect(frame, nullptr, this, nullptr);
}

void B3FlowMeshToasts::clear()
{
    const auto frames{std::move(m_frames)};
    m_frames.clear();
    for (const auto& frame : frames) {
        if (frame) delete frame.data();
    }
    m_keys.clear();
    m_key_order.clear();
    hide();
}

QStringList B3FlowMeshToasts::texts() const
{
    QStringList out;
    for (const auto& frame : m_frames) {
        if (!frame) continue;
        if (const auto* label{frame->findChild<QLabel*>()}) out << label->text();
    }
    return out;
}

void B3FlowMeshToasts::prune(const QObject* removed)
{
    m_frames.erase(std::remove_if(m_frames.begin(), m_frames.end(), [removed](const QPointer<QFrame>& frame) {
        return frame.isNull() || static_cast<const QObject*>(frame.data()) == removed;
    }), m_frames.end());
    setVisible(!m_frames.empty());
}

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3placeholderpage.h>

#include <qt/b3theme.h>

#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>

B3PlaceholderPage::B3PlaceholderPage(const QString& title, const QString& body, QWidget* parent)
    : QWidget{parent}
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(B3Theme::kSpaceXl, B3Theme::kSpaceXl, B3Theme::kSpaceXl,
                              B3Theme::kSpaceXl);

    auto* card = new QFrame(this);
    B3Theme::markCard(card);
    auto* inner = new QVBoxLayout(card);
    inner->setContentsMargins(B3Theme::kSpaceXl, B3Theme::kSpaceXl, B3Theme::kSpaceXl,
                              B3Theme::kSpaceXl);
    inner->setSpacing(B3Theme::kSpaceMd);

    auto* heading = new QLabel(title, card);
    B3Theme::markTextRole(heading, "h1");
    heading->setAccessibleName(title);

    m_body = new QLabel(body, card);
    B3Theme::markTextRole(m_body, "secondary");
    m_body->setWordWrap(true);

    m_note = new QLabel(card);
    B3Theme::markTextRole(m_note, "muted");
    m_note->setWordWrap(true);
    m_note->setVisible(false);

    inner->addWidget(heading);
    inner->addWidget(m_body);
    inner->addWidget(m_note);
    inner->addStretch(1);

    outer->addWidget(card);
    outer->addStretch(1);
}

void B3PlaceholderPage::setBody(const QString& body)
{
    m_body->setText(body);
}

void B3PlaceholderPage::setNote(const QString& note)
{
    m_note->setText(note);
    m_note->setVisible(!note.isEmpty());
}

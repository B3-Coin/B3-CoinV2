// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3THEME_H
#define BITCOIN_QT_B3THEME_H

#include <QColor>
#include <QString>

class QApplication;
class QWidget;

/**
 * Centralized B3 Hive visual system. A single source of truth for
 * colors, spacing, radii, typography and component styling so the UI reads
 * as one premium, calm, dark product rather than scattered stylesheet
 * strings. Everything here is presentation only.
 */
namespace B3Theme {

// Obsidian and honey-gold palette. These values mirror the approved visual
// direction and deliberately avoid the blue "generic wallet" appearance.
inline const QColor kBackground{0x0d, 0x0e, 0x0f};
inline const QColor kSurface{0x11, 0x12, 0x10};
inline const QColor kCard{0x14, 0x15, 0x13};
inline const QColor kCardHover{0x1b, 0x1b, 0x18};
inline const QColor kBorder{0x29, 0x29, 0x23};
inline const QColor kTextPrimary{0xf3, 0xf0, 0xe8};
inline const QColor kTextSecondary{0xaa, 0xa8, 0x9f};
inline const QColor kTextMuted{0x6f, 0x6e, 0x68};
inline const QColor kAccent{0xf1, 0xbd, 0x47};
inline const QColor kAccentMuted{0x28, 0x25, 0x1a};
inline const QColor kPositive{0x5b, 0xc8, 0x95};
inline const QColor kNegative{0xe1, 0x70, 0x72};
inline const QColor kWarning{0xe3, 0x9b, 0x43};      // testnet
inline const QColor kRegtest{0xa9, 0x7b, 0xe8};      // regtest

// Spacing / geometry scale (device-independent px).
inline constexpr int kSpaceXs{4};
inline constexpr int kSpaceSm{8};
inline constexpr int kSpaceMd{16};
inline constexpr int kSpaceLg{24};
inline constexpr int kSpaceXl{32};
inline constexpr int kRadiusSm{6};
inline constexpr int kRadiusMd{10};
inline constexpr int kRadiusLg{14};
inline constexpr int kBorderWidth{1};
inline constexpr int kIconSm{16};
inline constexpr int kIconMd{20};
inline constexpr int kIconLg{28};
inline constexpr int kSidebarWidth{202};
inline constexpr int kSidebarCompactWidth{70};

//! Whether the user requested reduced motion (env or platform hint). UI
//! animation must be suppressed when true.
bool reducedMotion();

//! The global application stylesheet implementing the visual system.
QString styleSheet();

//! Apply the palette + stylesheet to the application once at startup.
void apply(QApplication& app);

//! Tag a widget as a rounded card/panel (targeted by the stylesheet).
void markCard(QWidget* widget);
//! Text-role helpers (targeted by the stylesheet via a dynamic property).
void markTextRole(QWidget* widget, const QString& role);

} // namespace B3Theme

#endif // BITCOIN_QT_B3THEME_H

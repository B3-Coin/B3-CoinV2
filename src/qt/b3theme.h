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
 * Centralized B3FlowMesh visual system. A single source of truth for
 * colors, spacing, radii, typography and component styling so the UI reads
 * as one premium, calm, dark product rather than scattered stylesheet
 * strings. Everything here is presentation only.
 */
namespace B3Theme {

// Palette — soft dark neutral, restrained.
inline const QColor kBackground{0x14, 0x16, 0x1a};
inline const QColor kSurface{0x1a, 0x1d, 0x24};
inline const QColor kCard{0x21, 0x25, 0x2e};
inline const QColor kCardHover{0x27, 0x2c, 0x37};
inline const QColor kBorder{0x2b, 0x30, 0x3b};
inline const QColor kTextPrimary{0xe6, 0xe9, 0xef};
inline const QColor kTextSecondary{0x9a, 0xa3, 0xb2};
inline const QColor kTextMuted{0x6b, 0x72, 0x80};
inline const QColor kAccent{0x5a, 0x9c, 0xf8};       // calm blue
inline const QColor kAccentMuted{0x37, 0x4a, 0x66};
inline const QColor kPositive{0x4c, 0xc2, 0x8c};
inline const QColor kNegative{0xe0, 0x6c, 0x75};
inline const QColor kWarning{0xe0, 0x9a, 0x3a};      // testnet
inline const QColor kRegtest{0x9a, 0x6c, 0xe0};      // regtest

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
inline constexpr int kSidebarWidth{212};

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

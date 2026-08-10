// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3theme.h>

#include <QApplication>
#include <QByteArray>
#include <QPalette>
#include <QString>
#include <QWidget>

namespace B3Theme {

bool reducedMotion()
{
    // An explicit user/test setting wins over the platform heuristic.
    const QByteArray env{qgetenv("B3_REDUCED_MOTION")};
    if (env == "1" || env == "true") return true;
    if (env == "0" || env == "false") return false;
    // Offscreen/headless test platforms should not animate.
    if (qgetenv("QT_QPA_PLATFORM") == "offscreen") return true;
    return false;
}

static QString hex(const QColor& c) { return c.name(QColor::HexRgb); }

QString styleSheet()
{
    // One centralized stylesheet. Component styling keys off object names
    // and the dynamic "b3card"/"b3role" properties so widgets stay free of
    // inline stylesheet strings.
    return QStringLiteral(R"(
QWidget { background: %BG%; color: %TEXT%; font-size: 13px; }
QMainWindow, QDialog { background: %BG%; }

/* Rounded cards / panels */
QFrame[b3card="true"], QWidget[b3card="true"] {
    background: %CARD%;
    border: %BW%px solid %BORDER%;
    border-radius: %RMD%px;
}

/* Text roles */
QLabel[b3role="h1"] { color: %TEXT%; font-size: 22px; font-weight: 600; }
QLabel[b3role="h2"] { color: %TEXT%; font-size: 16px; font-weight: 600; }
QLabel[b3role="title"] { color: %TEXT%; font-size: 14px; font-weight: 600; }
QLabel[b3role="secondary"] { color: %TEXT2%; }
QLabel[b3role="muted"] { color: %MUTED%; font-size: 12px; }
QLabel[b3role="mono"] { color: %TEXT%; font-family: monospace; }
QLabel[b3role="accent"] { color: %ACCENT%; font-weight: 600; }

/* Left navigation sidebar */
#B3Sidebar { background: %SURFACE%; border-right: %BW%px solid %BORDER%; }
#B3Sidebar QToolButton {
    background: transparent; color: %TEXT2%;
    border: none; border-radius: %RSM%px;
    padding: 10px 12px; text-align: left; font-size: 13px;
}
#B3Sidebar QToolButton:hover { background: %CARDHOVER%; color: %TEXT%; }
#B3Sidebar QToolButton:checked { background: %ACCENTMUTED%; color: %TEXT%; font-weight: 600; }
#B3Sidebar QToolButton:focus { border: %BW%px solid %ACCENT%; }
#B3SidebarBrand { color: %TEXT%; font-size: 15px; font-weight: 700; }
#B3SidebarBrandSub { color: %MUTED%; font-size: 11px; }

/* Top status area */
#B3TopStatus { background: %SURFACE%; border-bottom: %BW%px solid %BORDER%; }
#B3NetBadge { border-radius: %RSM%px; padding: 2px 8px; font-size: 11px; font-weight: 600; }

QToolTip { background: %CARD%; color: %TEXT%; border: %BW%px solid %BORDER%; }

/* Buttons */
QPushButton {
    background: %CARD%; color: %TEXT%;
    border: %BW%px solid %BORDER%; border-radius: %RSM%px;
    padding: 7px 14px;
}
QPushButton:hover { background: %CARDHOVER%; }
QPushButton:focus { border: %BW%px solid %ACCENT%; }
QPushButton:disabled { color: %MUTED%; background: %SURFACE%; }
QPushButton[b3variant="primary"] { background: %ACCENT%; color: #0d1017; border: none; font-weight: 600; }
QPushButton[b3variant="primary"]:hover { background: %ACCENT%; }
QPushButton[b3variant="primary"]:disabled { background: %ACCENTMUTED%; color: %MUTED%; }

/* Inputs */
QLineEdit, QComboBox, QAbstractSpinBox {
    background: %BG%; color: %TEXT%;
    border: %BW%px solid %BORDER%; border-radius: %RSM%px; padding: 5px 8px;
    selection-background-color: %ACCENTMUTED%;
}
QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus { border: %BW%px solid %ACCENT%; }

/* Tables / lists */
QTableView, QListView, QTreeView {
    background: %CARD%; border: %BW%px solid %BORDER%; border-radius: %RMD%px;
    alternate-background-color: %SURFACE%;
    selection-background-color: %ACCENTMUTED%; selection-color: %TEXT%;
    gridline-color: %BORDER%;
}
QHeaderView::section {
    background: %SURFACE%; color: %TEXT2%; border: none;
    border-bottom: %BW%px solid %BORDER%; padding: 6px 8px;
}

/* Scrollbars */
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: %BORDER%; border-radius: 5px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: %MUTED%; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; }
QScrollBar::handle:horizontal { background: %BORDER%; border-radius: 5px; min-width: 24px; }

QProgressBar {
    background: %BG%; border: %BW%px solid %BORDER%; border-radius: %RSM%px;
    text-align: center; color: %TEXT2%; height: 8px;
}
QProgressBar::chunk { background: %ACCENT%; border-radius: %RSM%px; }
)")
        .replace("%BG%", hex(kBackground))
        .replace("%SURFACE%", hex(kSurface))
        .replace("%CARDHOVER%", hex(kCardHover))
        .replace("%CARD%", hex(kCard))
        .replace("%BORDER%", hex(kBorder))
        .replace("%TEXT2%", hex(kTextSecondary))
        .replace("%TEXT%", hex(kTextPrimary))
        .replace("%MUTED%", hex(kTextMuted))
        .replace("%ACCENTMUTED%", hex(kAccentMuted))
        .replace("%ACCENT%", hex(kAccent))
        .replace("%RSM%", QString::number(kRadiusSm))
        .replace("%RMD%", QString::number(kRadiusMd))
        .replace("%BW%", QString::number(kBorderWidth));
}

void apply(QApplication& app)
{
    QPalette palette;
    palette.setColor(QPalette::Window, kBackground);
    palette.setColor(QPalette::WindowText, kTextPrimary);
    palette.setColor(QPalette::Base, kBackground);
    palette.setColor(QPalette::AlternateBase, kSurface);
    palette.setColor(QPalette::Text, kTextPrimary);
    palette.setColor(QPalette::Button, kCard);
    palette.setColor(QPalette::ButtonText, kTextPrimary);
    palette.setColor(QPalette::Highlight, kAccentMuted);
    palette.setColor(QPalette::HighlightedText, kTextPrimary);
    palette.setColor(QPalette::ToolTipBase, kCard);
    palette.setColor(QPalette::ToolTipText, kTextPrimary);
    palette.setColor(QPalette::PlaceholderText, kTextMuted);
    palette.setColor(QPalette::Link, kAccent);
    palette.setColor(QPalette::Disabled, QPalette::Text, kTextMuted);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, kTextMuted);
    app.setPalette(palette);
    app.setStyleSheet(styleSheet());
}

void markCard(QWidget* widget)
{
    if (widget) widget->setProperty("b3card", true);
}

void markTextRole(QWidget* widget, const QString& role)
{
    if (widget) widget->setProperty("b3role", role);
}

} // namespace B3Theme

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
    // and dynamic properties so widgets stay free of inline style strings.
    return QStringLiteral(R"(
/* The palette supplies defaults. Do not paint every child QWidget: native
   and legacy forms use widgets as transparent layout containers. */
QWidget { color: %TEXT%; font-size: 13px; }
QWidget:window, QMainWindow, QDialog, #B3Shell, #B3ShellMain, #B3Content {
    background: %BG%;
}
QLabel { background: transparent; }

/* Rounded cards / panels */
QFrame[b3card="true"], QWidget[b3card="true"] {
    background: %CARD%;
    border: %BW%px solid %BORDER%;
    border-radius: %RMD%px;
}
QFrame[b3surface="panel"], QWidget[b3surface="panel"] {
    background: %CARD%;
    border: %BW%px solid %BORDER%;
    border-radius: 12px;
}
QFrame[b3surface="quiet"], QWidget[b3surface="quiet"] {
    background: %SURFACE%;
    border: %BW%px solid %BORDER%;
    border-radius: %RMD%px;
}
QFrame[b3surface="hero"], QWidget[b3surface="hero"] {
    background: qradialgradient(cx:0.82, cy:0.30, radius:0.82,
                                fx:0.82, fy:0.30,
                                stop:0 #2a2415, stop:0.34 #191815,
                                stop:1 #121311);
    border: %BW%px solid #302e27;
    border-radius: %RLG%px;
}

/* Text roles */
QLabel[b3role="h1"] { color: %TEXT%; font-size: 22px; font-weight: 600; }
QLabel[b3role="h2"] { color: %TEXT%; font-size: 16px; font-weight: 600; }
QLabel[b3role="h3"] { color: %TEXT%; font-size: 14px; font-weight: 600; }
QLabel[b3role="title"] { color: %TEXT%; font-size: 14px; font-weight: 600; }
QLabel[b3role="eyebrow"] { color: %ACCENT%; font-size: 10px; font-weight: 700; }
QLabel[b3role="heroEyebrow"], QLabel[b3role="eyebrowMuted"] {
    color: %TEXT2%; font-size: 10px; font-weight: 700;
}
QLabel[b3role="status"] { color: %TEXT2%; font-size: 11px; }
QLabel[b3role="secondary"] { color: %TEXT2%; }
QLabel[b3role="muted"] { color: %MUTED%; font-size: 12px; }
QLabel[b3role="mono"] { color: %TEXT%; font-family: monospace; }
QLabel[b3role="accent"] { color: %ACCENT%; font-weight: 600; }
QLabel[b3role="positive"] { color: %POSITIVE%; font-weight: 600; }
QLabel[b3role="negative"] { color: %NEGATIVE%; font-weight: 600; }
QLabel[b3role="balance"] { color: %TEXT%; font-size: 36px; font-weight: 600; }
QLabel[b3role="balanceUnit"] { color: %TEXT2%; font-size: 15px; font-weight: 600; padding-bottom: 4px; }

/* Left navigation sidebar */
#B3Sidebar { background: %SURFACE%; border-right: %BW%px solid %BORDER%; }
#B3SidebarBrandRow, #B3SidebarMark { background: transparent; }
#B3Sidebar QToolButton {
    background: transparent; color: %TEXT2%;
    border: %BW%px solid transparent; border-radius: 9px;
    padding: 10px 11px; text-align: left; font-size: 12px; font-weight: 500;
}
#B3Sidebar QToolButton:hover { background: %CARDHOVER%; color: %TEXT%; }
#B3Sidebar QToolButton:checked { background: %ACCENTMUTED%; color: %ACCENT%; font-weight: 600; }
#B3Sidebar QToolButton:focus { border: %BW%px solid %ACCENT%; }
#B3SidebarBrand { color: %TEXT%; font-size: 14px; font-weight: 700; }
#B3SidebarBrandSub { color: %MUTED%; font-size: 9px; font-weight: 600; }
#B3SidebarPlatform {
    color: %MUTED%; font-size: 9px;
    border-top: %BW%px solid %BORDER%; padding-top: 12px;
}

/* Top status area */
#B3TopStatus { background: %SURFACE%; border-bottom: %BW%px solid %BORDER%; }
#B3TopStatusTitle { color: %TEXT%; font-size: 17px; font-weight: 600; }
#B3TopStatus QLabel[b3status="true"] { color: %TEXT2%; font-size: 11px; }
#B3NetBadge { border-radius: 16px; padding: 3px 7px; font-size: 10px; font-weight: 700; }
#B3NetBadge[b3network="mainnet"] { background: #1a211b; color: #76d5a5; border: %BW%px solid #2f4435; }
#B3NetBadge[b3network="testnet"] { background: #2b2116; color: %WARNING%; border: %BW%px solid #514029; }
#B3NetBadge[b3network="regtest"] { background: #251d30; color: %REGTEST%; border: %BW%px solid #49375e; }
#B3WalletSecurity {
    background: %CARD%; border: %BW%px solid %BORDER%; border-radius: 8px;
    padding: 0;
}
#dashboardNetworkState[b3state="synced"] { color: %POSITIVE%; }

/* Synchronization overlay: an explicit dark modal surface, independent of
   the legacy form's nested container hierarchy. */
#ModalOverlay { background: transparent; }
#ModalOverlay #bgWidget { background: rgba(4, 5, 6, 224); }
#ModalOverlay #contentWidget {
    background: %CARD%;
    border: %BW%px solid #343229;
    border-radius: %RLG%px;
}
#ModalOverlay #contentWidget QLabel { background: transparent; color: %TEXT2%; }
#ModalOverlay #infoTextStrong { color: %TEXT%; font-weight: 600; }
#ModalOverlay #labelNumberOfBlocksLeft,
#ModalOverlay #labelLastBlockTime,
#ModalOverlay #labelSyncDone,
#ModalOverlay #labelProgressIncrease,
#ModalOverlay #labelEstimatedTimeLeft { color: %MUTED%; font-weight: 600; }
#ModalOverlay #numberOfBlocksLeft,
#ModalOverlay #newestBlockDate,
#ModalOverlay #percentageProgress,
#ModalOverlay #progressIncreasePerH,
#ModalOverlay #expectedTimeLeft { color: %TEXT%; }
#ModalOverlay #warningIcon { background: transparent; border: none; padding: 4px; }
#ModalOverlay #closeButton { min-width: 74px; }

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
QPushButton[b3variant="primary"] { background: %ACCENT%; color: #17140d; border: none; font-weight: 700; }
QPushButton[b3variant="primary"]:hover { background: %ACCENT%; }
QPushButton[b3variant="primary"]:disabled { background: %ACCENTMUTED%; color: %MUTED%; }
QPushButton[b3variant="timeframe"] {
    min-width: 30px; padding: 4px 9px;
    background: transparent; color: %MUTED%; border-color: transparent;
}
QPushButton[b3variant="timeframe"]:checked {
    background: %ACCENTMUTED%; color: %ACCENT%; border-color: #4c4021;
}

/* Trading remains a deliberately inert preview. Make that boundary more
   prominent than any individual ticket control. */
#tradePreviewBadge {
    background: %ACCENTMUTED%; color: %ACCENT%;
    border: %BW%px solid #4c4021; border-radius: 10px;
    padding: 4px 9px; font-size: 9px; font-weight: 700;
}
#tradeInactiveMessage {
    background: #1d1a12; color: #d9c990;
    border: %BW%px solid #4c4021; border-radius: %RSM%px;
    padding: 9px 12px; font-size: 11px;
}

/* Inputs */
QLineEdit, QComboBox, QAbstractSpinBox {
    background: %SURFACE%; color: %TEXT%;
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

QGroupBox {
    background: %CARD%; border: %BW%px solid %BORDER%; border-radius: %RMD%px;
    margin-top: 10px; padding: 14px 12px 10px;
}
QGroupBox::title { color: %TEXT2%; subcontrol-origin: margin; left: 10px; padding: 0 5px; }
QTabWidget::pane { border: %BW%px solid %BORDER%; background: %CARD%; }
QTabBar::tab { background: %SURFACE%; color: %MUTED%; padding: 8px 14px; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: %ACCENT%; border-bottom-color: %ACCENT%; }

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
        .replace("%POSITIVE%", hex(kPositive))
        .replace("%NEGATIVE%", hex(kNegative))
        .replace("%WARNING%", hex(kWarning))
        .replace("%REGTEST%", hex(kRegtest))
        .replace("%RSM%", QString::number(kRadiusSm))
        .replace("%RMD%", QString::number(kRadiusMd))
        .replace("%RLG%", QString::number(kRadiusLg))
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

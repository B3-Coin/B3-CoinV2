// Copyright (c) 2015-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platformstyle.h>

#include <QApplication>
#include <QColor>
#include <QIconEngine>
#include <QImage>
#include <QPalette>
#include <QPainter>

#include <utility>

static const struct {
    const char *platformId;
    /** Show images on push buttons */
    const bool imagesOnButtons;
    /** Colorize single-color icons */
    const bool colorizeIcons;
    /** Extra padding/spacing in transactionview */
    const bool useExtraSpacing;
} platform_styles[] = {
    // B3 uses the same dark application theme on every platform. Inherited
    // macOS dialogs must not erase their icons, nor Windows retain black ink.
    {"macosx", true, true, true},
    {"windows", true, true, false},
    /* Other: linux, unix, ... */
    {"other", true, true, false}
};

namespace {
/* Local functions for colorizing single-color images */

void MakeSingleColorImage(QImage& img, const QColor& colorbase)
{
    img = img.convertToFormat(QImage::Format_ARGB32);
    for (int x = img.width(); x--; )
    {
        for (int y = img.height(); y--; )
        {
            const QRgb rgb = img.pixel(x, y);
            img.setPixel(x, y, qRgba(colorbase.red(), colorbase.green(), colorbase.blue(), qAlpha(rgb)));
        }
    }
}

class MonochromeIconEngine final : public QIconEngine
{
public:
    MonochromeIconEngine(QIcon source, QColor foreground)
        : m_source{std::move(source)}, m_foreground{std::move(foreground)} {}

    QIconEngine* clone() const override { return new MonochromeIconEngine{*this}; }
    bool isNull() override { return m_source.isNull(); }
    QList<QSize> availableSizes(QIcon::Mode mode, QIcon::State state) override
    {
        return m_source.availableSizes(mode, state);
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        const QPixmap image{Render(rect.size(), painter->device()->devicePixelRatioF(), mode, state)};
        painter->drawPixmap(rect, image);
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return Render(size, 1.0, mode, state);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        // Before Qt 6.8, QIcon passed device pixels to this hook instead of
        // logical pixels. Avoid applying the display scale twice there.
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
        return Render((QSizeF{size} / scale).toSize(), scale, mode, state);
#else
        return Render(size, scale, mode, state);
#endif
    }

private:
    QPixmap Render(const QSize& size, qreal scale, QIcon::Mode mode, QIcon::State state) const
    {
        // Keep the source engine alive: scalable/native icons may report no
        // availableSizes(), and the On and Off states may use different art.
        // We supply the disabled appearance ourselves below. Some native
        // styles generate a Disabled source pixmap at half opacity; tinting
        // that again would double-dim the glyph into the dark background.
        // Preserve the On/Off artwork, but use its undimmed source alpha.
        const QIcon::Mode source_mode{mode == QIcon::Disabled ? QIcon::Normal : mode};
        QImage image{m_source.pixmap(size, scale, source_mode, state).toImage()};
        if (image.isNull()) return {};
        const QColor color{mode == QIcon::Disabled
                               ? QApplication::palette().color(QPalette::Disabled, QPalette::WindowText)
                               : m_foreground};
        MakeSingleColorImage(image, color);
        return QPixmap::fromImage(image);
    }

    QIcon m_source;
    QColor m_foreground;
};

QIcon ColorizeIcon(const QIcon& icon, const QColor& colorbase)
{
    return icon.isNull() ? QIcon{} : QIcon{new MonochromeIconEngine{icon, colorbase}};
}

QImage ColorizeImage(const QString& filename, const QColor& colorbase)
{
    QImage img(filename);
    MakeSingleColorImage(img, colorbase);
    return img;
}

QIcon ColorizeIcon(const QString& filename, const QColor& colorbase)
{
    return ColorizeIcon(QIcon{filename}, colorbase);
}

}


PlatformStyle::PlatformStyle(const QString &_name, bool _imagesOnButtons, bool _colorizeIcons, bool _useExtraSpacing):
    name(_name),
    imagesOnButtons(_imagesOnButtons),
    colorizeIcons(_colorizeIcons),
    useExtraSpacing(_useExtraSpacing)
{
}

QColor PlatformStyle::TextColor() const
{
    return QApplication::palette().color(QPalette::WindowText);
}

QColor PlatformStyle::SingleColor() const
{
    if (colorizeIcons) {
        QColor colorHighlightBg(QApplication::palette().color(QPalette::Highlight));
        QColor colorHighlightFg(QApplication::palette().color(QPalette::HighlightedText));
        const QColor colorText(QApplication::palette().color(QPalette::WindowText));
        const int colorTextLightness = colorText.lightness();
        if (abs(colorHighlightBg.lightness() - colorTextLightness) < abs(colorHighlightFg.lightness() - colorTextLightness)) {
            return colorHighlightBg;
        }
        return colorHighlightFg;
    }
    return {0, 0, 0};
}

QImage PlatformStyle::SingleColorImage(const QString& filename) const
{
    if (!colorizeIcons)
        return QImage(filename);
    return ColorizeImage(filename, SingleColor());
}

QIcon PlatformStyle::SingleColorIcon(const QString& filename) const
{
    if (!colorizeIcons)
        return QIcon(filename);
    return ColorizeIcon(filename, SingleColor());
}

QIcon PlatformStyle::SingleColorIcon(const QIcon& icon) const
{
    if (!colorizeIcons)
        return icon;
    return ColorizeIcon(icon, SingleColor());
}

QIcon PlatformStyle::TextColorIcon(const QString& filename) const
{
    return ColorizeIcon(filename, TextColor());
}

QIcon PlatformStyle::TextColorIcon(const QIcon& icon) const
{
    return ColorizeIcon(icon, TextColor());
}

const PlatformStyle *PlatformStyle::instantiate(const QString &platformId)
{
    for (const auto& platform_style : platform_styles) {
        if (platformId == platform_style.platformId) {
            return new PlatformStyle(
                    platform_style.platformId,
                    platform_style.imagesOnButtons,
                    platform_style.colorizeIcons,
                    platform_style.useExtraSpacing);
        }
    }
    return nullptr;
}

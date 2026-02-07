#include "StyledComboBox.h"
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QAbstractItemView>
#include <QGraphicsDropShadowEffect>
#include "../core/ThemeManager.h"

StyledComboBox::StyledComboBox(QWidget* parent)
    : QComboBox(parent)
{
    setObjectName(QStringLiteral("StyledComboBox"));
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(UISizes::inputHeight);

    refreshTheme();

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &StyledComboBox::refreshTheme);
}

void StyledComboBox::refreshTheme()
{
    auto c = ThemeManager::instance()->colors();

    QString bg       = c.backgroundTertiary;
    QString border   = c.border;
    QString hoverBg  = c.hover;
    QString hoverBd  = c.pressed;
    QString focusBd  = c.borderFocus;
    QString text     = c.foreground;
    QString popupBg  = c.backgroundElevated;
    QString popupBd  = c.borderSubtle;
    QString itemText = c.foregroundSecondary;
    QString itemHov  = c.hover;
    QString itemSel  = c.selected;
    QString selText  = c.foreground;

    setStyleSheet(QStringLiteral(
        "StyledComboBox {"
        "  background: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 6px;"
        "  padding: 0px 12px;"
        "  padding-right: 30px;"
        "  color: %3;"
        "  font-size: 13px;"
        "  font-weight: 500;"
        "}"
        "StyledComboBox:hover {"
        "  background: %4;"
        "  border-color: %5;"
        "}"
        "StyledComboBox:focus {"
        "  border-color: %6;"
        "}"
        "StyledComboBox::drop-down {"
        "  subcontrol-origin: padding;"
        "  subcontrol-position: center right;"
        "  width: 24px; border: none; background: transparent;"
        "}"
        "StyledComboBox::down-arrow {"
        "  image: none; width: 0; height: 0;"
        "}"
        "StyledComboBox QAbstractItemView {"
        "  background: %7;"
        "  border: 1px solid %8;"
        "  border-radius: 6px;"
        "  padding: 6px;"
        "  outline: none;"
        "  selection-background-color: transparent;"
        "}"
        "StyledComboBox QAbstractItemView::item {"
        "  height: 32px;"
        "  padding: 6px 10px;"
        "  border-radius: 4px;"
        "  color: %9;"
        "  background: transparent;"
        "}"
        "StyledComboBox QAbstractItemView::item:hover {"
        "  background: %10;"
        "}"
        "StyledComboBox QAbstractItemView::item:selected {"
        "  background: %11;"
        "  color: %12;"
        "}")
            .arg(bg, border, text, hoverBg, hoverBd, focusBd, popupBg, popupBd, itemText)
            .arg(itemHov, itemSel, selText));

    // Style the popup view directly for consistent rendering
    if (view()) {
        view()->setStyleSheet(QStringLiteral(
            "QListView {"
            "  background: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 6px;"
            "  padding: 6px;"
            "  outline: none;"
            "}"
            "QListView::item {"
            "  height: 32px;"
            "  padding-left: 10px;"
            "  border-radius: 4px;"
            "  color: %3;"
            "}"
            "QListView::item:hover {"
            "  background: %4;"
            "}"
            "QListView::item:selected {"
            "  background: %5;"
            "  color: %6;"
            "}")
                .arg(popupBg, popupBd, itemText, itemHov, itemSel, selText));

        // Add shadow to the popup for depth
        auto* shadow = new QGraphicsDropShadowEffect(view());
        shadow->setBlurRadius(24);
        shadow->setOffset(0, 4);
        shadow->setColor(ThemeColors::toQColor(c.shadowLight));
        view()->setGraphicsEffect(shadow);
    }
}

void StyledComboBox::paintEvent(QPaintEvent* /*event*/)
{
    QStylePainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Draw the combo box frame and background using the style
    QStyleOptionComboBox opt;
    initStyleOption(&opt);
    opt.subControls &= ~QStyle::SC_ComboBoxArrow;
    painter.drawComplexControl(QStyle::CC_ComboBox, opt);

    // Draw the current text
    painter.drawControl(QStyle::CE_ComboBoxLabel, opt);

    // Draw custom downward chevron arrow
    auto cc = ThemeManager::instance()->colors();
    const int arrowSize = 8;
    const int arrowMarginRight = 12;
    const int centerY = height() / 2;
    const int arrowX = width() - arrowMarginRight - arrowSize;

    QPainterPath chevronPath;
    chevronPath.moveTo(arrowX, centerY - arrowSize / 4.0);
    chevronPath.lineTo(arrowX + arrowSize / 2.0, centerY + arrowSize / 4.0);
    chevronPath.lineTo(arrowX + arrowSize, centerY - arrowSize / 4.0);

    QPen chevronPen(ThemeColors::toQColor(cc.foregroundSecondary));
    chevronPen.setWidthF(1.5);
    chevronPen.setCapStyle(Qt::RoundCap);
    chevronPen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(chevronPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(chevronPath);
}

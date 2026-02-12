#include "StyledMessageBox.h"
#include "../../core/ThemeManager.h"
#include "../../widgets/StyledButton.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QGraphicsDropShadowEffect>
#include <QPainter>
#include <QKeyEvent>

// ═════════════════════════════════════════════════════════════════════
//  Constructor
// ═════════════════════════════════════════════════════════════════════

StyledMessageBox::StyledMessageBox(QWidget* parent)
    : QDialog(parent)
{
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);
    setupUi();
}

// ═════════════════════════════════════════════════════════════════════
//  setupUi
// ═════════════════════════════════════════════════════════════════════

void StyledMessageBox::setupUi()
{
    auto tc = ThemeManager::instance()->colors();

    // ── Outer layout (provides space for drop shadow) ────────────────
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(24, 24, 24, 24);

    // ── Main container with rounded corners ──────────────────────────
    auto* container = new QWidget(this);
    container->setObjectName(QStringLiteral("StyledMessageBoxContainer"));
    container->setStyleSheet(QStringLiteral(
        "QWidget#StyledMessageBoxContainer {"
        "  background: %1;"
        "  border-radius: 16px;"
        "  border: 1px solid %2;"
        "}"
    ).arg(tc.backgroundElevated, tc.border));

    auto* shadow = new QGraphicsDropShadowEffect(container);
    shadow->setBlurRadius(40);
    shadow->setXOffset(0);
    shadow->setYOffset(8);
    shadow->setColor(QColor(0, 0, 0, 150));
    container->setGraphicsEffect(shadow);

    outerLayout->addWidget(container);

    // ── Container layout ─────────────────────────────────────────────
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(0);

    // ── Icon ─────────────────────────────────────────────────────────
    m_iconLabel = new QLabel(container);
    m_iconLabel->setFixedSize(56, 56);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    m_iconLabel->hide();
    layout->addWidget(m_iconLabel, 0, Qt::AlignCenter);
    layout->addSpacing(20);

    // ── Title ────────────────────────────────────────────────────────
    m_titleLabel = new QLabel(container);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->setWordWrap(true);
    m_titleLabel->setStyleSheet(QStringLiteral(
        "font-size: 18px; font-weight: 600; color: %1; background: transparent;"
    ).arg(tc.foreground));
    layout->addWidget(m_titleLabel);
    layout->addSpacing(12);

    // ── Message ──────────────────────────────────────────────────────
    m_messageLabel = new QLabel(container);
    m_messageLabel->setAlignment(Qt::AlignCenter);
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setMaximumWidth(300);
    // Set font via setFont() so font() returns correct size for sizeHint/layout
    QFont msgFont = m_messageLabel->font();
    msgFont.setPixelSize(14);
    m_messageLabel->setFont(msgFont);
    m_messageLabel->setStyleSheet(QStringLiteral(
        "color: %1; background: transparent; padding: 4px 0px;"
    ).arg(tc.foregroundSecondary));
    layout->addWidget(m_messageLabel, 0, Qt::AlignCenter);
    layout->addSpacing(28);

    // ── Button container ─────────────────────────────────────────────
    m_buttonContainer = new QWidget(container);
    m_buttonContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    m_buttonLayout = new QHBoxLayout(m_buttonContainer);
    m_buttonLayout->setContentsMargins(0, 0, 0, 0);
    m_buttonLayout->setSpacing(12);
    m_buttonLayout->addStretch();
    layout->addWidget(m_buttonContainer);

    setMinimumWidth(380);
    setMinimumHeight(280);
}

// ═════════════════════════════════════════════════════════════════════
//  setTitle / setMessage
// ═════════════════════════════════════════════════════════════════════

void StyledMessageBox::setTitle(const QString& title)
{
    m_titleLabel->setText(title);
}

void StyledMessageBox::setMessage(const QString& message)
{
    m_messageLabel->setText(message);
    m_messageLabel->setVisible(!message.isEmpty());
}

// ═════════════════════════════════════════════════════════════════════
//  setIcon — draw simple vector icons via QPainter
// ═════════════════════════════════════════════════════════════════════

void StyledMessageBox::setIcon(Icon icon)
{
    if (icon == Icon::None) {
        m_iconLabel->hide();
        return;
    }

    auto tc = ThemeManager::instance()->colors();
    QString iconColor;
    QString bgColor;

    bool dark = ThemeManager::instance()->isDark();
    double bgOpacity = dark ? 0.20 : 0.12;

    switch (icon) {
    case Icon::Question:
        iconColor = tc.accent;
        bgColor = QStringLiteral("rgba(100, 140, 255, %1)").arg(bgOpacity);
        break;
    case Icon::Warning:
        iconColor = tc.warning;
        bgColor = QStringLiteral("rgba(245, 158, 11, %1)").arg(bgOpacity);
        break;
    case Icon::Error:
        iconColor = tc.error;
        bgColor = QStringLiteral("rgba(239, 68, 68, %1)").arg(bgOpacity);
        break;
    case Icon::Info:
        iconColor = tc.accent;
        bgColor = QStringLiteral("rgba(100, 140, 255, %1)").arg(bgOpacity);
        break;
    default:
        m_iconLabel->hide();
        return;
    }

    m_iconLabel->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: 28px;"
    ).arg(bgColor));

    // Paint a simple icon
    qreal dpr = devicePixelRatioF();
    QPixmap pixmap(QSize(32, 32) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    QColor color(iconColor);

    switch (icon) {
    case Icon::Question: {
        QFont f(QStringLiteral(".AppleSystemUIFont"), 20, QFont::DemiBold);
        p.setFont(f);
        p.setPen(color);
        p.drawText(QRect(0, 0, 32, 32), Qt::AlignCenter, QStringLiteral("?"));
        break;
    }
    case Icon::Warning: {
        QFont f(QStringLiteral(".AppleSystemUIFont"), 22, QFont::Bold);
        p.setFont(f);
        p.setPen(color);
        p.drawText(QRect(0, 0, 32, 32), Qt::AlignCenter, QStringLiteral("!"));
        break;
    }
    case Icon::Error: {
        QPen pen(color, 2.5);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(9, 9, 23, 23);
        p.drawLine(23, 9, 9, 23);
        break;
    }
    case Icon::Info: {
        QFont f(QStringLiteral(".AppleSystemUIFont"), 20, QFont::DemiBold);
        p.setFont(f);
        p.setPen(color);
        p.drawText(QRect(0, 0, 32, 32), Qt::AlignCenter, QStringLiteral("i"));
        break;
    }
    default:
        break;
    }

    m_iconLabel->setPixmap(pixmap);
    m_iconLabel->show();
}

// ═════════════════════════════════════════════════════════════════════
//  addButton
// ═════════════════════════════════════════════════════════════════════

void StyledMessageBox::addButton(ButtonType type, bool isPrimary)
{
    auto* btn = new StyledButton(buttonText(type),
                                  isPrimary ? QStringLiteral("default")
                                            : QStringLiteral("outline"),
                                  m_buttonContainer);

    ButtonVariant variant;
    if (type == ButtonType::Delete) {
        variant = ButtonVariant::Destructive;
    } else if (isPrimary) {
        variant = ButtonVariant::Primary;
    } else {
        variant = ButtonVariant::Secondary;
    }

    btn->setFixedHeight(40);
    btn->setMinimumWidth(100);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(ThemeManager::instance()->buttonStyle(variant)
        + QStringLiteral(
        " QPushButton {"
        "  min-height: 40px; max-height: 40px;"
        "  padding: 0 20px;"
        "  font-size: 14px; font-weight: 500;"
        "  border-radius: 10px;"
        "}"));

    connect(btn, &QPushButton::clicked, this, [this, type]() {
        m_clickedButton = type;
        accept();
    });

    m_buttonLayout->addWidget(btn);

    // Keep a trailing stretch so buttons stay grouped
    m_buttonLayout->addStretch();
}

// ═════════════════════════════════════════════════════════════════════
//  buttonText
// ═════════════════════════════════════════════════════════════════════

QString StyledMessageBox::buttonText(ButtonType type) const
{
    switch (type) {
    case ButtonType::Ok:      return QStringLiteral("OK");
    case ButtonType::Cancel:  return QStringLiteral("Cancel");
    case ButtonType::Yes:     return QStringLiteral("Yes");
    case ButtonType::No:      return QStringLiteral("No");
    case ButtonType::Delete:  return QStringLiteral("Delete");
    case ButtonType::Save:    return QStringLiteral("Save");
    case ButtonType::Discard: return QStringLiteral("Discard");
    }
    return {};
}

// ═════════════════════════════════════════════════════════════════════
//  Static convenience methods
// ═════════════════════════════════════════════════════════════════════

bool StyledMessageBox::confirm(QWidget* parent, const QString& title,
                                const QString& message)
{
    StyledMessageBox box(parent);
    box.setIcon(Icon::Question);
    box.setTitle(title);
    box.setMessage(message);
    box.addButton(ButtonType::No, false);
    box.addButton(ButtonType::Yes, true);
    box.adjustSize();
    if (parent) box.move(parent->geometry().center() - box.rect().center());
    box.exec();
    return box.clickedButton() == ButtonType::Yes;
}

bool StyledMessageBox::confirmDelete(QWidget* parent, const QString& itemName)
{
    StyledMessageBox box(parent);
    box.setIcon(Icon::Warning);
    box.setTitle(QStringLiteral("Delete \"%1\"?").arg(itemName));
    box.setMessage(QStringLiteral("This action cannot be undone."));
    box.addButton(ButtonType::Cancel, false);
    box.addButton(ButtonType::Delete, true);
    box.adjustSize();
    if (parent) box.move(parent->geometry().center() - box.rect().center());
    box.exec();
    return box.clickedButton() == ButtonType::Delete;
}

void StyledMessageBox::info(QWidget* parent, const QString& title,
                             const QString& message)
{
    StyledMessageBox box(parent);
    box.setIcon(Icon::Info);
    box.setTitle(title);
    box.setMessage(message);
    box.addButton(ButtonType::Ok, true);
    box.adjustSize();
    if (parent) box.move(parent->geometry().center() - box.rect().center());
    box.exec();
}

void StyledMessageBox::warning(QWidget* parent, const QString& title,
                                const QString& message)
{
    StyledMessageBox box(parent);
    box.setIcon(Icon::Warning);
    box.setTitle(title);
    box.setMessage(message);
    box.addButton(ButtonType::Ok, true);
    box.adjustSize();
    if (parent) box.move(parent->geometry().center() - box.rect().center());
    box.exec();
}

void StyledMessageBox::error(QWidget* parent, const QString& title,
                              const QString& message)
{
    StyledMessageBox box(parent);
    box.setIcon(Icon::Error);
    box.setTitle(title);
    box.setMessage(message);
    box.addButton(ButtonType::Ok, true);
    box.adjustSize();
    if (parent) box.move(parent->geometry().center() - box.rect().center());
    box.exec();
}

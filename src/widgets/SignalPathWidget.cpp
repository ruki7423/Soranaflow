#include "SignalPathWidget.h"
#include "../core/ThemeManager.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QMouseEvent>

// ── Helper: quality color from theme ────────────────────────────────
static QString qualityColor(SignalPathNode::Quality q, const ThemeColors& c)
{
    switch (q) {
    case SignalPathNode::BitPerfect: return QStringLiteral("#B57EDC"); // purple
    case SignalPathNode::Lossless: return c.success;       // green
    case SignalPathNode::HighRes:  return c.badgeHires;    // blue
    case SignalPathNode::Enhanced: return c.accent;        // blue/accent
    case SignalPathNode::Lossy:   return c.warning;       // yellow/orange
    default:                      return c.foregroundMuted; // gray
    }
}

// ═════════════════════════════════════════════════════════════════════
//  Constructor
// ═════════════════════════════════════════════════════════════════════

SignalPathWidget::SignalPathWidget(QWidget* parent)
    : QWidget(parent)
{
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);
    m_mainLayout->setSpacing(0);

    // ── Header row ──────────────────────────────────────────────────
    m_headerWidget = new QWidget(this);
    m_headerWidget->setCursor(Qt::PointingHandCursor);
    auto* headerLayout = new QHBoxLayout(m_headerWidget);
    headerLayout->setContentsMargins(0, 8, 0, 8);
    headerLayout->setSpacing(8);

    m_chevronLabel = new QLabel(QStringLiteral("\u25BC"), m_headerWidget); // ▼
    m_chevronLabel->setFixedWidth(16);

    m_headerLabel = new QLabel(QStringLiteral("Signal Path"), m_headerWidget);

    m_qualityBadge = new QLabel(m_headerWidget);
    m_qualityBadge->setAlignment(Qt::AlignCenter);
    m_qualityBadge->setFixedHeight(20);
    m_qualityBadge->hide();

    headerLayout->addWidget(m_chevronLabel);
    headerLayout->addWidget(m_headerLabel);
    headerLayout->addStretch();
    headerLayout->addWidget(m_qualityBadge);

    m_mainLayout->addWidget(m_headerWidget);

    // ── Node container ──────────────────────────────────────────────
    m_nodeContainer = new QWidget(this);
    m_nodeLayout = new QVBoxLayout(m_nodeContainer);
    m_nodeLayout->setContentsMargins(8, 0, 0, 0);
    m_nodeLayout->setSpacing(0);

    m_mainLayout->addWidget(m_nodeContainer);

    // ── Click handler on header ─────────────────────────────────────
    m_headerWidget->installEventFilter(this);

    // ── Theme ───────────────────────────────────────────────────────
    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SignalPathWidget::refreshTheme);
    refreshTheme();
}

// ═════════════════════════════════════════════════════════════════════
//  Public API
// ═════════════════════════════════════════════════════════════════════

void SignalPathWidget::updateSignalPath(const SignalPathInfo& info)
{
    m_info = info;
    rebuild();
    show();
}

void SignalPathWidget::clear()
{
    m_info = SignalPathInfo{};
    // Remove all node widgets but keep the widget visible (header stays)
    QLayoutItem* child;
    while ((child = m_nodeLayout->takeAt(0)) != nullptr) {
        if (child->widget()) {
            child->widget()->deleteLater();
        }
        delete child;
    }
    m_qualityBadge->hide();
}

void SignalPathWidget::setCollapsed(bool collapsed)
{
    m_collapsed = collapsed;
    m_nodeContainer->setVisible(!collapsed);
    m_chevronLabel->setText(collapsed ? QStringLiteral("\u25B6")   // ▶
                                      : QStringLiteral("\u25BC")); // ▼
}

// ═════════════════════════════════════════════════════════════════════
//  rebuild — recreate node widgets from m_info
// ═════════════════════════════════════════════════════════════════════

void SignalPathWidget::rebuild()
{
    auto c = ThemeManager::instance()->colors();

    // Clear existing nodes
    QLayoutItem* child;
    while ((child = m_nodeLayout->takeAt(0)) != nullptr) {
        if (child->widget()) child->widget()->deleteLater();
        delete child;
    }

    // Quality badge
    if (!m_info.nodes.isEmpty()) {
        auto overall = m_info.overallQuality();
        QString qLabel = SignalPathInfo::qualityLabel(overall);
        QString qColor = qualityColor(overall, c);

        m_qualityBadge->setText(qLabel);
        m_qualityBadge->setStyleSheet(
            QStringLiteral("QLabel {"
                           "  background-color: %1;"
                           "  color: %2;"
                           "  font-size: 10px;"
                           "  font-weight: bold;"
                           "  padding: 2px 8px;"
                           "  border-radius: 3px;"
                           "}").arg(qColor, c.badgeText.isEmpty()
                                       ? QStringLiteral("#FFFFFF") : c.badgeText));
        m_qualityBadge->adjustSize();
        m_qualityBadge->show();
    } else {
        m_qualityBadge->hide();
    }

    // Build node chain
    for (int i = 0; i < m_info.nodes.size(); ++i) {
        const auto& node = m_info.nodes[i];

        auto* nodeWidget = new QWidget(m_nodeContainer);
        auto* nodeLayout = new QHBoxLayout(nodeWidget);
        nodeLayout->setContentsMargins(0, 6, 0, 6);
        nodeLayout->setSpacing(8);

        // Colored quality dot
        auto* dot = new QLabel(nodeWidget);
        dot->setFixedSize(10, 10);
        QString dotColor = qualityColor(node.quality, c);
        dot->setStyleSheet(
            QStringLiteral("QLabel {"
                           "  background-color: %1;"
                           "  border-radius: 5px;"
                           "  border: none;"
                           "}").arg(dotColor));

        // Text column
        auto* textWidget = new QWidget(nodeWidget);
        auto* textLayout = new QVBoxLayout(textWidget);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(1);

        auto* labelText = new QLabel(node.label, textWidget);
        labelText->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; font-weight: bold; border: none;")
                .arg(c.foreground));

        textLayout->addWidget(labelText);

        if (!node.detail.isEmpty()) {
            auto* detailText = new QLabel(node.detail, textWidget);
            detailText->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px; border: none;")
                    .arg(c.foregroundSecondary));
            textLayout->addWidget(detailText);
        }

        if (!node.sublabel.isEmpty()) {
            auto* sublabelText = new QLabel(node.sublabel, textWidget);
            sublabelText->setStyleSheet(
                QStringLiteral("color: %1; font-size: 10px; border: none;")
                    .arg(c.foregroundMuted));
            textLayout->addWidget(sublabelText);
        }

        nodeLayout->addWidget(dot, 0, Qt::AlignTop);
        nodeLayout->addWidget(textWidget, 1);

        m_nodeLayout->addWidget(nodeWidget);

        // Connecting line between nodes (except after last)
        if (i < m_info.nodes.size() - 1) {
            auto* line = new QWidget(m_nodeContainer);
            line->setFixedSize(2, 12);
            line->setStyleSheet(
                QStringLiteral("background-color: %1;").arg(c.borderSubtle));

            auto* lineContainer = new QWidget(m_nodeContainer);
            auto* lineLayout = new QHBoxLayout(lineContainer);
            lineLayout->setContentsMargins(4, 0, 0, 0); // align with dot center
            lineLayout->setSpacing(0);
            lineLayout->addWidget(line);
            lineLayout->addStretch();

            m_nodeLayout->addWidget(lineContainer);
        }
    }

    m_nodeContainer->setVisible(!m_collapsed);
}

// ═════════════════════════════════════════════════════════════════════
//  refreshTheme
// ═════════════════════════════════════════════════════════════════════

void SignalPathWidget::refreshTheme()
{
    auto c = ThemeManager::instance()->colors();

    m_headerLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; font-weight: bold; border: none;")
            .arg(c.foreground));

    m_chevronLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 10px; border: none;")
            .arg(c.foregroundMuted));

    // Rebuild nodes with new theme colors
    if (!m_info.nodes.isEmpty()) {
        rebuild();
    }
}

// ═════════════════════════════════════════════════════════════════════
//  eventFilter — toggle collapse on header click
// ═════════════════════════════════════════════════════════════════════

bool SignalPathWidget::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_headerWidget && event->type() == QEvent::MouseButtonRelease) {
        setCollapsed(!m_collapsed);
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

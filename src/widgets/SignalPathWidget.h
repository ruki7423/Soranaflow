#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include "../core/audio/SignalPathInfo.h"

class SignalPathWidget : public QWidget {
    Q_OBJECT
public:
    explicit SignalPathWidget(QWidget* parent = nullptr);

    void updateSignalPath(const SignalPathInfo& info);
    void clear();

    void setCollapsed(bool collapsed);
    bool isCollapsed() const { return m_collapsed; }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void refreshTheme();
    void rebuild();

    QVBoxLayout* m_mainLayout = nullptr;

    // Header row
    QWidget*  m_headerWidget = nullptr;
    QLabel*   m_headerLabel  = nullptr;
    QLabel*   m_chevronLabel = nullptr;
    QLabel*   m_qualityBadge = nullptr;

    // Node container
    QWidget*     m_nodeContainer = nullptr;
    QVBoxLayout* m_nodeLayout    = nullptr;

    SignalPathInfo m_info;
    bool m_collapsed = false;
};

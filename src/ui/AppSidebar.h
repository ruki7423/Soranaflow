#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QPropertyAnimation>
#include <QLabel>
#include <QScrollArea>
#include <QEvent>
#include <QMouseEvent>
#include "SoranaFlowLogo.h"
#include "../widgets/StyledButton.h"
#include "../widgets/StyledInput.h"

class AppSidebar : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int sidebarWidth READ sidebarWidth WRITE setSidebarWidth)
public:
    explicit AppSidebar(QWidget* parent = nullptr);
    int sidebarWidth() const { return width(); }
    void setSidebarWidth(int w) { setFixedWidth(w); }
    bool isCollapsed() const { return m_collapsed; }
    void rebuildFolderButtons();
    void focusSearch();
    void clearSearch();

signals:
    void navigationChanged(int index);
    void collapseToggled(bool collapsed);
    void folderSelected(const QString& folderPath);
    void searchRequested(const QString& query);

public slots:
    void toggleCollapse();
    void setActiveIndex(int index);

private slots:
    void refreshTheme();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void setupUI();
    QPushButton* createNavButton(const QString& text, const QString& iconPath);
    void updateNavStyles();

    bool m_collapsed = false;
    int m_activeIndex = 0;
    QVBoxLayout* m_mainLayout;
    QWidget* m_logoBar;
    StyledInput* m_searchInput;
    QWidget* m_navContainer;
    QScrollArea* m_navScroll;
    QWidget* m_librarySection;
    QPushButton* m_settingsButton;
    QVector<QPushButton*> m_navButtons;
    QVector<QPushButton*> m_folderButtons;
    QVBoxLayout* m_libLayout = nullptr;
    QPropertyAnimation* m_collapseAnim;
    QLabel* m_logoLabel;
    QPushButton* m_collapsedSearchBtn = nullptr;
};

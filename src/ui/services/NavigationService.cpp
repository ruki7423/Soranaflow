#include "NavigationService.h"
#include "../MainWindow.h"

NavigationService::NavigationService(QObject* parent)
    : QObject(parent)
{
    // Forward MainWindow's globalNavChanged → our navChanged
    if (auto* mw = MainWindow::instance()) {
        connect(mw, &MainWindow::globalNavChanged,
                this, &NavigationService::navChanged);
    }
}

NavigationService* NavigationService::instance()
{
    static NavigationService s;
    return &s;
}

void NavigationService::navigateBack()
{
    if (auto* mw = MainWindow::instance())
        mw->navigateBack();
}

void NavigationService::navigateForward()
{
    if (auto* mw = MainWindow::instance())
        mw->navigateForward();
}

bool NavigationService::canGoBack() const
{
    auto* mw = MainWindow::instance();
    return mw && mw->canGoBack();
}

bool NavigationService::canGoForward() const
{
    auto* mw = MainWindow::instance();
    return mw && mw->canGoForward();
}

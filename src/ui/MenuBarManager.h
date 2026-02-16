#pragma once

#include <QObject>

class QMainWindow;

class MenuBarManager : public QObject {
    Q_OBJECT
public:
    explicit MenuBarManager(QMainWindow* window);

signals:
    void quitRequested();
    void focusSearchRequested();
};

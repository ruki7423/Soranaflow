#pragma once
#include <QObject>

class NavigationService : public QObject {
    Q_OBJECT
public:
    static NavigationService* instance();

    void navigateBack();
    void navigateForward();
    bool canGoBack() const;
    bool canGoForward() const;

signals:
    void navChanged();

private:
    explicit NavigationService(QObject* parent = nullptr);
};

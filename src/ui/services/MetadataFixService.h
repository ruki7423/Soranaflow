#pragma once

#include <QObject>
#include <QList>

class TrackTableView;

class MetadataFixService : public QObject {
    Q_OBJECT
public:
    explicit MetadataFixService(QObject* parent = nullptr);
    void connectToTable(TrackTableView* table, QWidget* dialogParent);
    void disconnectFromTable(TrackTableView* table);

signals:
    void metadataUpdated(QList<int> trackIds);
};

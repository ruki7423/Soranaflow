#pragma once

#include <QWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QLabel>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>

class AMContentPanel : public QWidget {
    Q_OBJECT
public:
    explicit AMContentPanel(QWidget* parent = nullptr);
    void clear();
    void refreshScrollStyle();

signals:
    void songPlayRequested(const QJsonObject& song);
    void artistNavigationRequested(const QString& id, const QString& name);
    void albumNavigationRequested(const QString& id, const QString& name, const QString& artist);
    void songContextMenuRequested(const QPoint& globalPos, const QJsonObject& song);

protected:
    void buildSongsSection(const QString& header, const QJsonArray& songs);
    void buildAlbumsGrid(const QString& header, const QJsonArray& albums);
    void buildArtistsGrid(const QString& header, const QJsonArray& artists);

    QWidget* createSongRow(const QJsonObject& song);
    QWidget* createAlbumCard(const QJsonObject& album, int cardWidth);
    QWidget* createArtistCard(const QJsonObject& artist, int cardWidth);
    QWidget* createSectionHeader(const QString& title);
    void loadArtwork(const QString& url, QLabel* target, int size, bool circular = false);
    bool eventFilter(QObject* obj, QEvent* event) override;

    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_resultsContainer = nullptr;
    QVBoxLayout* m_resultsLayout = nullptr;
    QNetworkAccessManager* m_networkManager = nullptr;

    QObject* m_lastClickedRow = nullptr;
    qint64 m_lastClickTime = 0;
    qint64 m_lastPlayTime = 0;
};

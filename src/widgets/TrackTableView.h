#pragma once

#include <QTableView>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QHeaderView>
#include <QPainter>
#include <QMouseEvent>
#include "../core/MusicData.h"

// ── Column configuration ────────────────────────────────────────────
enum class TrackColumn {
    Number,
    Title,
    Artist,
    Album,
    Format,
    Duration
};

struct TrackTableConfig {
    QString settingsKey;
    QVector<TrackColumn> columns;
};

// Preset configs
inline TrackTableConfig libraryConfig() {
    return { QStringLiteral("library"),
             { TrackColumn::Number, TrackColumn::Title, TrackColumn::Artist,
               TrackColumn::Album, TrackColumn::Format, TrackColumn::Duration } };
}

inline TrackTableConfig albumDetailConfig() {
    return { QStringLiteral("albumDetail"),
             { TrackColumn::Number, TrackColumn::Title,
               TrackColumn::Format, TrackColumn::Duration } };
}

inline TrackTableConfig artistDetailConfig() {
    return { QStringLiteral("artistDetail"),
             { TrackColumn::Number, TrackColumn::Title, TrackColumn::Album,
               TrackColumn::Format, TrackColumn::Duration } };
}

inline TrackTableConfig playlistDetailConfig() {
    return { QStringLiteral("playlistDetail"),
             { TrackColumn::Number, TrackColumn::Title, TrackColumn::Artist,
               TrackColumn::Album, TrackColumn::Format, TrackColumn::Duration } };
}

// ── Delegate ────────────────────────────────────────────────────────
class TrackTableDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit TrackTableDelegate(const QVector<TrackColumn>& columns,
                                QObject* parent = nullptr);

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

    void setHoverRow(int row) { m_hoverRow = row; }
    void setHighlightedTrackId(const QString& id) { m_highlightedId = id; }

private:
    QColor resolveFormatColor(AudioFormat format,
                              const QString& sampleRate,
                              const QString& bitDepth) const;

    QVector<TrackColumn> m_columns;
    int m_hoverRow = -1;
    QString m_highlightedId;
};

// ── Table view ──────────────────────────────────────────────────────
class TrackTableView : public QTableView
{
    Q_OBJECT
public:
    explicit TrackTableView(const TrackTableConfig& config,
                            QWidget* parent = nullptr);

    void setTracks(const QVector<Track>& tracks);
    void setHighlightedTrackId(const QString& id);
    const QVector<Track>& tracks() const { return m_tracks; }

    // For embedding inside scroll areas: disable internal scrollbar,
    // set fixed height to fit all rows
    void setEmbeddedMode(bool embedded);

signals:
    void trackClicked(const Track& track);
    void trackDoubleClicked(const Track& track);
    void editTagsRequested(const Track& track);
    void fixMetadataRequested(const Track& track);
    void undoMetadataRequested(const Track& track);
    void identifyByAudioRequested(const Track& track);
    void artistClicked(const QString& artistName);
    void albumClicked(const QString& albumName);

protected:
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void setupHeader();
    void refreshTheme();
    void saveColumnWidths();
    void restoreColumnWidths();
    int columnForTrackColumn(TrackColumn col) const;
    void populateModel();
    void onHeaderClicked(int logicalIndex);

    TrackTableConfig m_config;
    QStandardItemModel* m_model;
    TrackTableDelegate* m_delegate;
    QVector<Track> m_tracks;
    QString m_highlightedId;
    bool m_embedded = false;
    int m_hoverRow = -1;
    TrackColumn m_sortColumn = TrackColumn::Number;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    bool m_sorted = false;
};

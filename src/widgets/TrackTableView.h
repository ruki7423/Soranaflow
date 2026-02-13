#pragma once

#include <QTableView>
#include <QAbstractTableModel>
#include <QStyledItemDelegate>
#include <QHeaderView>
#include <QPainter>
#include <QMouseEvent>
#include <QTimer>
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

// ── Hybrid track model ──────────────────────────────────────────────
// Stores TrackIndex master array + display list for filtering/sorting.
// Supports both: setIndexes() for library, setTracks() for detail views.
class HybridTrackModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    explicit HybridTrackModel(const QVector<TrackColumn>& columns, QObject* parent = nullptr);

    // Set data from TrackIndex array (LibraryView — lightweight)
    void setIndexes(QVector<TrackIndex> indexes);

    // Set data from Track array (detail views — converts to TrackIndex)
    void setTracks(const QVector<Track>& tracks);

    // Filtering via display list
    void setFilter(const QString& query);
    void setFilterArtist(const QString& artist);
    void setFilterAlbum(const QString& album);
    void setFilterFolder(const QString& folder);
    void clearFilter();

    // Sorting via display list
    void sortByColumn(TrackColumn col, Qt::SortOrder order);
    void clearSort();

    // Access
    const TrackIndex& indexAt(int displayRow) const;
    int masterIndexForRow(int displayRow) const;
    int visibleCount() const { return m_displayList.size(); }

    void setHeaderSuffix(int section, const QString& suffix);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    void rebuildDisplayList();
    void applySortToDisplayList();

    QVector<TrackIndex> m_master;
    QVector<int> m_displayList;    // indices into m_master
    QVector<TrackColumn> m_columns;
    QVector<QString> m_headerSuffixes;

    // Filter state
    enum class FilterMode { None, Search, Artist, Album, Folder };
    FilterMode m_filterMode = FilterMode::None;
    QString m_filterValue;

    // Sort state
    TrackColumn m_sortColumn = TrackColumn::Number;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    bool m_sorted = false;
};

// ── Table view ──────────────────────────────────────────────────────
class TrackTableView : public QTableView
{
    Q_OBJECT
public:
    explicit TrackTableView(const TrackTableConfig& config,
                            QWidget* parent = nullptr);

    // Detail views (small track count) — model converts to TrackIndex
    void setTracks(const QVector<Track>& tracks);

    // Library view (80K+) — model stores TrackIndex directly
    void setIndexes(QVector<TrackIndex> indexes);

    void setHighlightedTrackId(const QString& id);

    // Access to model for filter/sort delegation
    HybridTrackModel* hybridModel() const { return m_model; }

    // Visible count (after filtering)
    int visibleCount() const { return m_model->visibleCount(); }

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
    void onHeaderClicked(int logicalIndex);
    Track trackForDisplayRow(int row) const;

    TrackTableConfig m_config;
    HybridTrackModel* m_model;
    TrackTableDelegate* m_delegate;

    // Full Track storage for detail views (setTracks path)
    QVector<Track> m_fullTracks;
    bool m_hasFullTracks = false;

    QString m_highlightedId;
    bool m_embedded = false;
    int m_hoverRow = -1;
    TrackColumn m_sortColumn = TrackColumn::Number;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    bool m_sorted = false;
    QTimer* m_columnSaveTimer = nullptr;
};

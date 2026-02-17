#include "TrackTableView.h"
#include "../core/ThemeManager.h"
#include "../core/Settings.h"
#include "../core/PlaybackState.h"
#include "../core/library/PlaylistManager.h"
#include "../core/library/LibraryDatabase.h"
#include "../ui/dialogs/NewPlaylistDialog.h"
#include "../platform/macos/MacUtils.h"

#include <QMenu>
#include <QSettings>
#include <QScrollBar>
#include <QContextMenuEvent>
#include <QTimer>
#include <QSet>
#include <algorithm>

// ═════════════════════════════════════════════════════════════════════
//  TrackTableDelegate
// ═════════════════════════════════════════════════════════════════════

TrackTableDelegate::TrackTableDelegate(const QVector<TrackColumn>& columns,
                                       QObject* parent)
    : QStyledItemDelegate(parent)
    , m_columns(columns)
{
}

QSize TrackTableDelegate::sizeHint(const QStyleOptionViewItem& /*option*/,
                                   const QModelIndex& /*index*/) const
{
    return QSize(100, UISizes::rowHeight);
}

void TrackTableDelegate::paint(QPainter* painter,
                               const QStyleOptionViewItem& option,
                               const QModelIndex& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    auto c = ThemeManager::instance()->colors();
    const int row = index.row();
    const int col = index.column();

    // ── Resolved colors from theme ─────────────────────────────────
    const QColor fgColor   = ThemeColors::toQColor(c.foreground);
    const QColor mutedClr  = ThemeColors::toQColor(c.foregroundMuted);

    // ── Track id stored in column 0, UserRole+10 ──────────────────
    const QString trackId = index.model()->index(row, 0)
                                .data(Qt::UserRole + 10).toString();

    // ── Background — we paint ALL backgrounds ourselves ───────────
    const bool isHighlighted = (!m_highlightedId.isEmpty() && trackId == m_highlightedId);
    const bool isHovered = (row == m_hoverRow);
    const bool isSelected = option.state & QStyle::State_Selected;

    // Always clear background first to prevent Qt's default selection painting
    painter->fillRect(option.rect, Qt::transparent);

    if (isSelected) {
        painter->fillRect(option.rect, ThemeColors::toQColor(c.selected));
    } else if (isHighlighted) {
        QColor hlColor = ThemeColors::toQColor(c.selected);
        hlColor.setAlpha(hlColor.alpha() * 3 / 4);
        painter->fillRect(option.rect, hlColor);
    } else if (isHovered) {
        painter->fillRect(option.rect, ThemeColors::toQColor(c.hover));
    }

    // ── Left accent bar for playing track ─────────────────────────
    if (isHighlighted && col == 0) {
        painter->fillRect(QRect(option.rect.left(), option.rect.top() + 4,
                                3, option.rect.height() - 8),
                          QColor(c.selectedBorder));
    }

    // ── Determine which TrackColumn this visual column maps to ───
    if (col < 0 || col >= m_columns.size()) {
        painter->restore();
        return;
    }

    const TrackColumn tcol = m_columns[col];
    const QRect rect = option.rect.adjusted(8, 0, -8, 0);
    const QString text = index.data(Qt::DisplayRole).toString();

    QFont font = option.font;
    font.setPointSize(12);

    switch (tcol) {
    case TrackColumn::Number: {
        font.setPointSize(11);
        painter->setFont(font);
        painter->setPen(mutedClr);

        if (isHovered) {
            painter->setPen(fgColor);
            painter->drawText(rect, Qt::AlignCenter, QStringLiteral("\u25B6"));
        } else {
            painter->drawText(rect, Qt::AlignCenter, text);
        }
        break;
    }
    case TrackColumn::Title: {
        font.setBold(true);
        font.setPointSize(12);
        painter->setFont(font);
        painter->setPen(isHighlighted ? QColor(c.accent) : fgColor);
        const QString elided = painter->fontMetrics().elidedText(text, Qt::ElideRight, rect.width());
        painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, elided);
        break;
    }
    case TrackColumn::Artist:
    case TrackColumn::AlbumArtist:
    case TrackColumn::Album:
    case TrackColumn::Composer: {
        font.setPointSize(12);
        if (isHovered) font.setUnderline(true);
        painter->setFont(font);
        painter->setPen(isHovered ? ThemeColors::toQColor(c.accent) : mutedClr);
        const QString elided = painter->fontMetrics().elidedText(text, Qt::ElideRight, rect.width());
        painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, elided);
        break;
    }
    case TrackColumn::Format: {
        // Draw quality badge + format specs
        const auto format = static_cast<AudioFormat>(
            index.data(Qt::UserRole + 1).toInt());
        const QString sampleRate = index.data(Qt::UserRole + 2).toString();
        const QString bitDepth = index.data(Qt::UserRole + 3).toString();

        // Classify quality from format + metadata
        const AudioQuality quality = classifyAudioQuality(format, sampleRate, bitDepth);
        const QString qualityLabel = getQualityLabel(quality);
        const QColor badgeColor = (quality == AudioQuality::Unknown)
            ? QColor(0x95, 0xA5, 0xA6)
            : getQualityColor(quality);

        // Badge rect
        font.setBold(true);
        font.setPointSize(9);
        painter->setFont(font);
        QFontMetrics fmBadge(font);
        const int badgeW = fmBadge.horizontalAdvance(qualityLabel) + 12;
        const int badgeH = 18;
        const int badgeY = rect.center().y() - badgeH / 2;

        if (!qualityLabel.isEmpty()) {
            QRectF badgeRect(rect.left(), badgeY, badgeW, badgeH);
            painter->setBrush(badgeColor);
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(badgeRect, 3, 3);

            painter->setPen(QColor(c.badgeText));
            painter->drawText(badgeRect, Qt::AlignCenter, qualityLabel);
        }

        // Specs text after badge: "FLAC • 96 kHz / 24-bit"
        const int specsX = qualityLabel.isEmpty() ? rect.left() : rect.left() + badgeW + 6;
        const int specsW = rect.width() - (specsX - rect.left());
        if (specsW > 20) {
            font.setBold(false);
            font.setPointSize(9);
            painter->setFont(font);
            painter->setPen(mutedClr);

            QString specs = getFormatLabel(format);
            if (!sampleRate.isEmpty()) {
                specs += QStringLiteral(" \u2022 ") + sampleRate;
            }
            if (!bitDepth.isEmpty()) {
                specs += QStringLiteral(" / ") + bitDepth;
            }

            QRect specsRect(specsX, rect.top(), specsW, rect.height());
            const QString elidedSpecs = painter->fontMetrics().elidedText(
                specs, Qt::ElideRight, specsRect.width());
            painter->drawText(specsRect, Qt::AlignLeft | Qt::AlignVCenter, elidedSpecs);
        }
        break;
    }
    case TrackColumn::Duration: {
        font.setPointSize(11);
        painter->setFont(font);
        painter->setPen(mutedClr);
        painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, text);
        break;
    }
    }

    painter->restore();
}

QColor TrackTableDelegate::resolveFormatColor(AudioFormat format,
                                              const QString& sampleRate,
                                              const QString& bitDepth) const
{
    // Delegate to quality-based coloring
    return getQualityColor(classifyAudioQuality(format, sampleRate, bitDepth));
}

// ═════════════════════════════════════════════════════════════════════
//  HybridTrackModel — display list model over TrackIndex master array
// ═════════════════════════════════════════════════════════════════════

static const TrackIndex s_emptyIndex;

HybridTrackModel::HybridTrackModel(const QVector<TrackColumn>& columns, QObject* parent)
    : QAbstractTableModel(parent)
    , m_columns(columns)
    , m_headerSuffixes(columns.size())
{
}

void HybridTrackModel::setIndexes(QVector<TrackIndex> indexes)
{
    m_master = std::move(indexes);
    m_sorted = false;
    m_filterMode = FilterMode::None;
    m_filterValue.clear();
    rebuildDisplayList();
}

void HybridTrackModel::setTracks(const QVector<Track>& tracks)
{
    m_master.clear();
    m_master.reserve(tracks.size());
    for (const Track& t : tracks)
        m_master.append(indexFromTrack(t));
    m_sorted = false;
    m_filterMode = FilterMode::None;
    m_filterValue.clear();
    rebuildDisplayList();
}

void HybridTrackModel::setFilter(const QString& query)
{
    m_filterMode = query.isEmpty() ? FilterMode::None : FilterMode::Search;
    m_filterValue = query;

    if (m_filterMode == FilterMode::Search && query.length() >= 2) {
        // Use FTS5 for 2+ character queries (< 1ms vs ~50ms linear scan)
        auto* db = LibraryDatabase::instance();
        QVector<QString> matchIds = db->searchTracksFTS(query);
        QSet<QString> matchSet(matchIds.begin(), matchIds.end());

        beginResetModel();
        m_displayList.clear();
        m_displayList.reserve(matchIds.size());
        for (int i = 0; i < m_master.size(); ++i) {
            if (matchSet.contains(m_master[i].id))
                m_displayList.append(i);
        }
        if (m_sorted)
            applySortToDisplayList();
        endResetModel();
    } else {
        // Empty or 1-char: fallback to in-memory linear scan
        rebuildDisplayList();
    }
}

void HybridTrackModel::setFilterArtist(const QString& artist)
{
    m_filterMode = FilterMode::Artist;
    m_filterValue = artist;
    rebuildDisplayList();
}

void HybridTrackModel::setFilterAlbum(const QString& album)
{
    m_filterMode = FilterMode::Album;
    m_filterValue = album;
    rebuildDisplayList();
}

void HybridTrackModel::setFilterFolder(const QString& folder)
{
    m_filterMode = FilterMode::Folder;
    m_filterValue = folder;
    rebuildDisplayList();
}

void HybridTrackModel::clearFilter()
{
    m_filterMode = FilterMode::None;
    m_filterValue.clear();
    rebuildDisplayList();
}

void HybridTrackModel::sortByColumn(TrackColumn col, Qt::SortOrder order)
{
    m_sortColumn = col;
    m_sortOrder = order;
    m_sorted = true;
    // Re-sort display list in place (no full rebuild needed)
    beginResetModel();
    applySortToDisplayList();
    endResetModel();
}

void HybridTrackModel::clearSort()
{
    m_sorted = false;
    rebuildDisplayList();
}

const TrackIndex& HybridTrackModel::indexAt(int displayRow) const
{
    if (displayRow < 0 || displayRow >= m_displayList.size())
        return s_emptyIndex;
    return m_master[m_displayList[displayRow]];
}

int HybridTrackModel::masterIndexForRow(int displayRow) const
{
    if (displayRow < 0 || displayRow >= m_displayList.size())
        return -1;
    return m_displayList[displayRow];
}

void HybridTrackModel::setHeaderSuffix(int section, const QString& suffix)
{
    if (section < 0 || section >= m_headerSuffixes.size()) return;
    m_headerSuffixes[section] = suffix;
    emit headerDataChanged(Qt::Horizontal, section, section);
}

int HybridTrackModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return m_displayList.size();
}

int HybridTrackModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return m_columns.size();
}

QVariant HybridTrackModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) return {};
    const int row = index.row();
    const int col = index.column();
    if (row < 0 || row >= m_displayList.size() || col < 0 || col >= m_columns.size())
        return {};

    const TrackIndex& t = m_master[m_displayList[row]];

    // Track ID available on every column for highlight lookup
    if (role == Qt::UserRole + 10)
        return t.id;

    const TrackColumn tcol = m_columns[col];

    switch (tcol) {
    case TrackColumn::Number:
        if (role == Qt::DisplayRole) return QString::number(row + 1);
        if (role == Qt::TextAlignmentRole) return Qt::AlignCenter;
        break;
    case TrackColumn::Title:
        if (role == Qt::DisplayRole) return t.title;
        break;
    case TrackColumn::Artist:
        if (role == Qt::DisplayRole) return t.artist;
        break;
    case TrackColumn::AlbumArtist:
        if (role == Qt::DisplayRole)
            return t.albumArtist.isEmpty() ? t.artist : t.albumArtist;
        break;
    case TrackColumn::Album:
        if (role == Qt::DisplayRole) return t.album;
        break;
    case TrackColumn::Composer:
        if (role == Qt::DisplayRole) return t.composer;
        break;
    case TrackColumn::Format:
        if (role == Qt::DisplayRole) return getFormatLabel(t.format);
        if (role == Qt::UserRole + 1) return static_cast<int>(t.format);
        if (role == Qt::UserRole + 2) return t.sampleRate;
        if (role == Qt::UserRole + 3) return t.bitDepth;
        break;
    case TrackColumn::Duration:
        if (role == Qt::DisplayRole) return formatDuration(t.duration);
        if (role == Qt::TextAlignmentRole)
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        break;
    }

    return {};
}

QVariant HybridTrackModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    if (section < 0 || section >= m_columns.size())
        return {};

    QString label;
    switch (m_columns[section]) {
    case TrackColumn::Number:      label = QStringLiteral("#"); break;
    case TrackColumn::Title:       label = QStringLiteral("TITLE"); break;
    case TrackColumn::Artist:      label = QStringLiteral("ARTIST"); break;
    case TrackColumn::AlbumArtist: label = QStringLiteral("ALBUM ARTIST"); break;
    case TrackColumn::Album:       label = QStringLiteral("ALBUM"); break;
    case TrackColumn::Composer:    label = QStringLiteral("COMPOSER"); break;
    case TrackColumn::Format:      label = QStringLiteral("FORMAT"); break;
    case TrackColumn::Duration:    label = QStringLiteral("DURATION"); break;
    }

    if (section < m_headerSuffixes.size() && !m_headerSuffixes[section].isEmpty())
        label += m_headerSuffixes[section];

    return label;
}

void HybridTrackModel::rebuildDisplayList()
{
    beginResetModel();
    m_displayList.clear();
    m_displayList.reserve(m_master.size());

    for (int i = 0; i < m_master.size(); ++i) {
        const TrackIndex& ti = m_master[i];
        bool match = true;

        switch (m_filterMode) {
        case FilterMode::None:
            break;
        case FilterMode::Search:
            match = ti.title.contains(m_filterValue, Qt::CaseInsensitive) ||
                    ti.artist.contains(m_filterValue, Qt::CaseInsensitive) ||
                    ti.album.contains(m_filterValue, Qt::CaseInsensitive) ||
                    ti.composer.contains(m_filterValue, Qt::CaseInsensitive);
            break;
        case FilterMode::Artist:
            match = (ti.artist == m_filterValue);
            break;
        case FilterMode::Album:
            match = (ti.album == m_filterValue);
            break;
        case FilterMode::Folder:
            match = ti.filePath.startsWith(m_filterValue);
            break;
        }

        if (match) m_displayList.append(i);
    }

    if (m_sorted)
        applySortToDisplayList();

    endResetModel();
}

void HybridTrackModel::applySortToDisplayList()
{
    std::stable_sort(m_displayList.begin(), m_displayList.end(),
        [this](int a, int b) {
            const TrackIndex& ta = m_master[a];
            const TrackIndex& tb = m_master[b];
            bool less = false;
            switch (m_sortColumn) {
            case TrackColumn::Number:
                less = ta.trackNumber < tb.trackNumber;
                break;
            case TrackColumn::Title:
                less = ta.title.compare(tb.title, Qt::CaseInsensitive) < 0;
                break;
            case TrackColumn::Artist:
                less = ta.artist.compare(tb.artist, Qt::CaseInsensitive) < 0;
                break;
            case TrackColumn::AlbumArtist: {
                const QString& aa = ta.albumArtist.isEmpty() ? ta.artist : ta.albumArtist;
                const QString& ab = tb.albumArtist.isEmpty() ? tb.artist : tb.albumArtist;
                less = aa.compare(ab, Qt::CaseInsensitive) < 0;
                break;
            }
            case TrackColumn::Album:
                less = ta.album.compare(tb.album, Qt::CaseInsensitive) < 0;
                break;
            case TrackColumn::Composer:
                less = ta.composer.compare(tb.composer, Qt::CaseInsensitive) < 0;
                break;
            case TrackColumn::Format: {
                auto qa = classifyAudioQuality(ta.format, ta.sampleRate, ta.bitDepth);
                auto qb = classifyAudioQuality(tb.format, tb.sampleRate, tb.bitDepth);
                less = static_cast<int>(qa) < static_cast<int>(qb);
                break;
            }
            case TrackColumn::Duration:
                less = ta.duration < tb.duration;
                break;
            }
            return m_sortOrder == Qt::AscendingOrder ? less : !less;
        });
}

// ═════════════════════════════════════════════════════════════════════
//  TrackTableView
// ═════════════════════════════════════════════════════════════════════

TrackTableView::TrackTableView(const TrackTableConfig& config, QWidget* parent)
    : QTableView(parent)
    , m_config(config)
    , m_model(new HybridTrackModel(config.columns, this))
    , m_delegate(new TrackTableDelegate(config.columns, this))
{
    setModel(m_model);
    setItemDelegate(m_delegate);
    setMouseTracking(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setShowGrid(false);
    setAlternatingRowColors(false);
    setWordWrap(false);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setFocusPolicy(Qt::NoFocus);

    // Transparent background
    viewport()->setAutoFillBackground(false);
    setStyleSheet(QStringLiteral(
        "QTableView { background: transparent; border: none; outline: none; }"
        "QTableView::item { border: none; padding: 0; }"
        "QTableView::item:selected { background: transparent; }"
        "QTableView::item:focus { outline: none; border: none; }"));

    setupHeader();

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &TrackTableView::refreshTheme);

    // macOS: allow clicks on inactive window to pass through
    QTimer::singleShot(0, this, [this]() {
        enableAcceptsFirstMouse(this);
    });
}

void TrackTableView::setupHeader()
{
    auto* hdr = horizontalHeader();
    hdr->setHighlightSections(false);
    hdr->setSectionsMovable(false);
    hdr->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hdr->setFixedHeight(UISizes::headerHeight);
    hdr->setMinimumSectionSize(50);
    hdr->setTextElideMode(Qt::ElideNone);
    hdr->setSectionResizeMode(QHeaderView::Interactive);
    hdr->setStretchLastSection(false);

    // Find title column for stretch
    int titleCol = -1;
    for (int i = 0; i < m_config.columns.size(); ++i) {
        if (m_config.columns[i] == TrackColumn::Title) {
            titleCol = i;
            break;
        }
    }

    // Default column widths
    for (int i = 0; i < m_config.columns.size(); ++i) {
        switch (m_config.columns[i]) {
        case TrackColumn::Number:      hdr->resizeSection(i, 50); break;
        case TrackColumn::Title:       hdr->resizeSection(i, 300); break;
        case TrackColumn::Artist:      hdr->resizeSection(i, 180); break;
        case TrackColumn::AlbumArtist: hdr->resizeSection(i, 160); break;
        case TrackColumn::Album:       hdr->resizeSection(i, 180); break;
        case TrackColumn::Composer:    hdr->resizeSection(i, 160); break;
        case TrackColumn::Format:      hdr->resizeSection(i, 200); break;
        case TrackColumn::Duration:    hdr->resizeSection(i, 90); break;
        }
    }

    // Title column — Stretch to fill remaining space (prevents overflow)
    if (titleCol >= 0) {
        hdr->setSectionResizeMode(titleCol, QHeaderView::Stretch);
    }

    // Restore saved widths
    restoreColumnWidths();

    // Debounced save on resize — avoid disk I/O on every pixel of drag
    m_columnSaveTimer = new QTimer(this);
    m_columnSaveTimer->setSingleShot(true);
    m_columnSaveTimer->setInterval(300);
    connect(m_columnSaveTimer, &QTimer::timeout, this, &TrackTableView::saveColumnWidths);

    connect(hdr, &QHeaderView::sectionResized, this, [this]() {
        m_columnSaveTimer->start();  // restarts 300ms countdown
    });

    // Enable clickable headers for sorting
    hdr->setSectionsClickable(true);
    hdr->setSortIndicatorShown(false); // we use text-based arrows instead
    connect(hdr, &QHeaderView::sectionClicked,
            this, &TrackTableView::onHeaderClicked);

    // Style header
    refreshTheme();
}

void TrackTableView::refreshTheme()
{
    auto c = ThemeManager::instance()->colors();

    const QString headerStyle = QStringLiteral(
        "QHeaderView { background: transparent; border: none; }"
        "QHeaderView::section {"
        "  background: transparent;"
        "  color: %1;"
        "  font-size: 11px;"
        "  font-weight: bold;"
        "  text-transform: uppercase;"
        "  letter-spacing: 1px;"
        "  border: none;"
        "  border-bottom: 1px solid %2;"
        "  padding: 8px 8px;"
        "}"
        "QHeaderView::section:hover {"
        "  background: %3;"
        "}")
        .arg(c.foregroundMuted, c.border, c.hover);

    horizontalHeader()->setStyleSheet(headerStyle);

    // Update main table style
    setStyleSheet(QStringLiteral(
        "QTableView { background: transparent; border: none; outline: none; }"
        "QTableView::item { border: none; padding: 0; }"
        "QTableView::item:selected { background: transparent; }"
        "QTableView::item:focus { outline: none; border: none; }"));

    // Force repaint
    viewport()->update();
}

void TrackTableView::setTracks(const QVector<Track>& tracks)
{
    m_fullTracks = tracks;
    m_hasFullTracks = true;
    m_sorted = false;
    horizontalHeader()->setSortIndicatorShown(false);

    m_model->setTracks(tracks);

    // Uniform row height
    verticalHeader()->setDefaultSectionSize(UISizes::rowHeight);
    verticalHeader()->setVisible(false);

    // Embedded mode: update fixed height
    if (m_embedded) {
        const int totalHeight = m_model->rowCount() * UISizes::rowHeight
                              + horizontalHeader()->height() + 2;
        setFixedHeight(totalHeight);
    }
}

void TrackTableView::setIndexes(QVector<TrackIndex> indexes)
{
    m_fullTracks.clear();
    m_hasFullTracks = false;
    m_sorted = false;
    horizontalHeader()->setSortIndicatorShown(false);

    m_model->setIndexes(std::move(indexes));

    // Uniform row height
    verticalHeader()->setDefaultSectionSize(UISizes::rowHeight);
    verticalHeader()->setVisible(false);

    // Embedded mode: update fixed height
    if (m_embedded) {
        const int totalHeight = m_model->rowCount() * UISizes::rowHeight
                              + horizontalHeader()->height() + 2;
        setFixedHeight(totalHeight);
    }
}

void TrackTableView::onHeaderClicked(int logicalIndex)
{
    if (logicalIndex < 0 || logicalIndex >= m_config.columns.size())
        return;

    TrackColumn col = m_config.columns[logicalIndex];

    // Toggle order if same column, else ascending
    if (m_sorted && col == m_sortColumn) {
        m_sortOrder = (m_sortOrder == Qt::AscendingOrder)
                    ? Qt::DescendingOrder : Qt::AscendingOrder;
    } else {
        m_sortColumn = col;
        m_sortOrder = Qt::AscendingOrder;
    }
    m_sorted = true;

    // Update header arrow indicators
    for (int i = 0; i < m_config.columns.size(); ++i) {
        if (i == logicalIndex) {
            m_model->setHeaderSuffix(i, (m_sortOrder == Qt::AscendingOrder)
                ? QStringLiteral("  \u25B2")   // ▲
                : QStringLiteral("  \u25BC")); // ▼
        } else {
            m_model->setHeaderSuffix(i, QString());
        }
    }

    // Delegate sorting to the model's display list
    m_model->sortByColumn(col, m_sortOrder);
}

Track TrackTableView::trackForDisplayRow(int row) const
{
    if (m_hasFullTracks) {
        int masterIdx = m_model->masterIndexForRow(row);
        if (masterIdx >= 0 && masterIdx < m_fullTracks.size())
            return m_fullTracks[masterIdx];
    }
    // Index mode — construct Track from TrackIndex
    return trackFromIndex(m_model->indexAt(row));
}

void TrackTableView::setHighlightedTrackId(const QString& id)
{
    m_highlightedId = id;
    m_delegate->setHighlightedTrackId(id);
    viewport()->update();
}

void TrackTableView::setEmbeddedMode(bool embedded)
{
    m_embedded = embedded;
    if (embedded) {
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        const int totalHeight = m_model->rowCount() * UISizes::rowHeight
                              + horizontalHeader()->height() + 2;
        setFixedHeight(totalHeight);

        // Stretch Title column to fill available width
        for (int i = 0; i < m_config.columns.size(); ++i) {
            if (m_config.columns[i] == TrackColumn::Title) {
                horizontalHeader()->setSectionResizeMode(i, QHeaderView::Stretch);
                break;
            }
        }
    } else {
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMaximumHeight(QWIDGETSIZE_MAX);
    }
}

void TrackTableView::mouseMoveEvent(QMouseEvent* event)
{
    const QModelIndex idx = indexAt(event->pos());
    const int newRow = idx.isValid() ? idx.row() : -1;

    if (newRow != m_hoverRow) {
        m_hoverRow = newRow;
        m_delegate->setHoverRow(newRow);
        viewport()->update();
    }

    // Show pointing hand cursor over clickable columns (Artist, Album, #)
    if (idx.isValid()) {
        int artistCol = columnForTrackColumn(TrackColumn::Artist);
        int albumCol = columnForTrackColumn(TrackColumn::Album);
        int numCol = columnForTrackColumn(TrackColumn::Number);
        if (idx.column() == artistCol || idx.column() == albumCol || idx.column() == numCol) {
            viewport()->setCursor(Qt::PointingHandCursor);
        } else {
            viewport()->setCursor(Qt::ArrowCursor);
        }
    }

    QTableView::mouseMoveEvent(event);
}

void TrackTableView::leaveEvent(QEvent* event)
{
    if (m_hoverRow != -1) {
        m_hoverRow = -1;
        m_delegate->setHoverRow(-1);
        viewport()->update();
    }
    QTableView::leaveEvent(event);
}

void TrackTableView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const QModelIndex idx = indexAt(event->pos());
        if (idx.isValid() && idx.row() < m_model->visibleCount()) {
            const Track t = trackForDisplayRow(idx.row());

            // Click on # column (play icon area) -> play immediately
            int numCol = columnForTrackColumn(TrackColumn::Number);
            if (numCol >= 0 && idx.column() == numCol) {
                emit trackDoubleClicked(t);
                return;
            }

            // Click on Artist column -> navigate to artist filter
            int artistCol = columnForTrackColumn(TrackColumn::Artist);
            if (artistCol >= 0 && idx.column() == artistCol && !t.artist.isEmpty()) {
                emit artistClicked(t.artist);
                return;
            }

            // Click on Album column -> navigate to album filter
            int albumCol = columnForTrackColumn(TrackColumn::Album);
            if (albumCol >= 0 && idx.column() == albumCol && !t.album.isEmpty()) {
                emit albumClicked(t.album);
                return;
            }

            emit trackClicked(t);
        }
    }
    QTableView::mousePressEvent(event);
}

void TrackTableView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const QModelIndex idx = indexAt(event->pos());
        if (idx.isValid() && idx.row() < m_model->visibleCount()) {
            // Skip # column — single-click already triggered play in mousePressEvent
            int numCol = columnForTrackColumn(TrackColumn::Number);
            if (numCol >= 0 && idx.column() == numCol)
                return;

            emit trackDoubleClicked(trackForDisplayRow(idx.row()));
            return;
        }
    }
    QTableView::mouseDoubleClickEvent(event);
}

void TrackTableView::contextMenuEvent(QContextMenuEvent* event)
{
    const QModelIndex idx = indexAt(event->pos());
    if (!idx.isValid() || idx.row() >= m_model->visibleCount())
        return;

    // Collect selected tracks
    QVector<Track> selectedTracks;
    QModelIndexList selectedRows = selectionModel()->selectedRows();
    for (const QModelIndex& sel : selectedRows) {
        if (sel.row() >= 0 && sel.row() < m_model->visibleCount())
            selectedTracks.append(trackForDisplayRow(sel.row()));
    }
    if (selectedTracks.isEmpty())
        selectedTracks.append(trackForDisplayRow(idx.row()));

    const Track clickedTrack = trackForDisplayRow(idx.row());

    QMenu menu(this);
    menu.setStyleSheet(ThemeManager::instance()->menuStyle());

    QAction* play = menu.addAction(tr("Play"));
    QAction* playNext = menu.addAction(
        selectedTracks.size() > 1
            ? tr("Play Next (%1 Tracks)").arg(selectedTracks.size())
            : tr("Play Next"));
    QAction* addToQueue = menu.addAction(
        selectedTracks.size() > 1
            ? tr("Add to Queue (%1 Tracks)").arg(selectedTracks.size())
            : tr("Add to Queue"));
    menu.addSeparator();

    // ── Add to Playlist submenu ──
    QMenu* playlistMenu = menu.addMenu(
        selectedTracks.size() > 1
            ? tr("Add %1 Tracks to Playlist").arg(selectedTracks.size())
            : tr("Add to Playlist"));
    playlistMenu->setStyleSheet(ThemeManager::instance()->menuStyle());

    auto* pm = PlaylistManager::instance();
    QVector<Playlist> playlists = pm->allPlaylists();

    for (const Playlist& pl : playlists) {
        if (pl.isSmartPlaylist) continue;
        QAction* plAction = playlistMenu->addAction(pl.name);
        connect(plAction, &QAction::triggered, this, [selectedTracks, pl]() {
            for (const Track& trk : selectedTracks)
                PlaylistManager::instance()->addTrack(pl.id, trk);
        });
    }

    if (!playlists.isEmpty()) {
        playlistMenu->addSeparator();
    }

    QAction* newPlaylist = playlistMenu->addAction(tr("+ New Playlist..."));
    connect(newPlaylist, &QAction::triggered, this, [this, selectedTracks]() {
        NewPlaylistDialog dialog(window());
        if (dialog.exec() == QDialog::Accepted) {
            QString name = dialog.playlistName();
            if (!name.isEmpty()) {
                QString id = PlaylistManager::instance()->createPlaylist(name);
                if (!id.isEmpty()) {
                    for (const Track& trk : selectedTracks)
                        PlaylistManager::instance()->addTrack(id, trk);
                }
            }
        }
    });

    menu.addSeparator();

    QAction* editTags = menu.addAction(tr("Edit Tags..."));
    QAction* fixMeta = menu.addAction(tr("Fix Metadata..."));
    QAction* identifyAudio = menu.addAction(tr("Identify by Audio..."));

    QAction* undoMeta = menu.addAction(tr("Undo Metadata Changes"));
    bool hasBackup = LibraryDatabase::instance()->hasMetadataBackup(clickedTrack.id);
    undoMeta->setEnabled(hasBackup);

    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == editTags) {
        emit editTagsRequested(clickedTrack);
    } else if (chosen == fixMeta) {
        emit fixMetadataRequested(clickedTrack);
    } else if (chosen == identifyAudio) {
        emit identifyByAudioRequested(clickedTrack);
    } else if (chosen == undoMeta) {
        if (LibraryDatabase::instance()->undoLastMetadataChange(clickedTrack.id)) {
            qDebug() << "[MetadataUndo] Restored backup for track:" << clickedTrack.id;
            emit undoMetadataRequested(clickedTrack);
        }
    } else if (chosen == play) {
        emit trackDoubleClicked(clickedTrack);
    } else if (chosen == playNext) {
        if (selectedTracks.size() > 1)
            PlaybackState::instance()->insertNext(selectedTracks);
        else
            PlaybackState::instance()->insertNext(clickedTrack);
    } else if (chosen == addToQueue) {
        if (selectedTracks.size() > 1)
            PlaybackState::instance()->addToQueue(selectedTracks);
        else
            PlaybackState::instance()->addToQueue(clickedTrack);
    }
}

void TrackTableView::saveColumnWidths()
{
    QSettings settings(Settings::settingsPath(), QSettings::IniFormat);
    settings.setValue(
        QStringLiteral("trackTable/%1/headerState").arg(m_config.settingsKey),
        horizontalHeader()->saveState());
}

void TrackTableView::restoreColumnWidths()
{
    QSettings settings(Settings::settingsPath(), QSettings::IniFormat);
    const QByteArray state = settings.value(
        QStringLiteral("trackTable/%1/headerState").arg(m_config.settingsKey))
        .toByteArray();
    if (!state.isEmpty()) {
        horizontalHeader()->restoreState(state);

        // Re-apply Interactive on title column since restoreState overrides it
        int titleCol = columnForTrackColumn(TrackColumn::Title);
        if (titleCol >= 0) {
            horizontalHeader()->setSectionResizeMode(titleCol, QHeaderView::Interactive);
        }

        // Ensure duration column didn't shrink below readable width
        int durCol = columnForTrackColumn(TrackColumn::Duration);
        if (durCol >= 0 && horizontalHeader()->sectionSize(durCol) < 90) {
            horizontalHeader()->resizeSection(durCol, 90);
        }
    }
}

int TrackTableView::columnForTrackColumn(TrackColumn col) const
{
    for (int i = 0; i < m_config.columns.size(); ++i) {
        if (m_config.columns[i] == col)
            return i;
    }
    return -1;
}

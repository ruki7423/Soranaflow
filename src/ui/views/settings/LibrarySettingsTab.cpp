#include "LibrarySettingsTab.h"
#include "SettingsUtils.h"

#include "../../../core/ThemeManager.h"
#include "../../../core/Settings.h"
#include "../../../core/library/LibraryScanner.h"
#include "../../../core/library/LibraryDatabase.h"
#include "../../../core/MusicData.h"

#include "../../../widgets/StyledButton.h"
#include "../../../widgets/StyledSwitch.h"
#include "../../../widgets/StyledComboBox.h"
#include "../../../widgets/StyledScrollArea.h"

#include "../../dialogs/StyledMessageBox.h"

#ifdef Q_OS_MACOS
#include "../../../platform/macos/BookmarkManager.h"
#endif

#include <QFileDialog>
#include <QDir>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSignalBlocker>

// ═════════════════════════════════════════════════════════════════════
//  LibrarySettingsTab
// ═════════════════════════════════════════════════════════════════════

LibrarySettingsTab::LibrarySettingsTab(QWidget* parent)
    : QWidget(parent)
{
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* scrollArea = new StyledScrollArea();
    scrollArea->setWidgetResizable(true);

    auto* content = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 16, 12, 16);
    layout->setSpacing(0);

    // ── Section: Monitored Folders ─────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Monitored Folders")));

    // Dynamic folder list
    m_foldersContainer = new QWidget();
    m_foldersLayout = new QVBoxLayout(m_foldersContainer);
    m_foldersLayout->setContentsMargins(0, 0, 0, 0);
    m_foldersLayout->setSpacing(4);

    rebuildFolderList();

    layout->addWidget(m_foldersContainer);

    // Add Folder button
    auto* addFolderBtn = new StyledButton(QStringLiteral("Add Folder"),
                                           QStringLiteral("outline"));
    addFolderBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/folder.svg")));
    connect(addFolderBtn, &QPushButton::clicked, this, &LibrarySettingsTab::onAddFolderClicked);
    layout->addWidget(addFolderBtn);

    // ── Section: Scanning ──────────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Scanning")));

    auto* autoScanSwitch = new StyledSwitch();
    autoScanSwitch->setChecked(Settings::instance()->autoScanOnStartup());
    connect(autoScanSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setAutoScanOnStartup(checked);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Auto-scan on startup"),
        QString(),
        autoScanSwitch));

    auto* watchChangesSwitch = new StyledSwitch();
    watchChangesSwitch->setChecked(Settings::instance()->watchForChanges());
    connect(watchChangesSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setWatchForChanges(checked);
        LibraryScanner::instance()->setWatchEnabled(checked);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Watch for changes"),
        QStringLiteral("Automatically detect new files"),
        watchChangesSwitch));

    // Scan Now button + status
    auto* scanRow = new QWidget();
    auto* scanRowLayout = new QHBoxLayout(scanRow);
    scanRowLayout->setContentsMargins(0, 8, 0, 8);
    scanRowLayout->setSpacing(12);

    m_scanNowBtn = new StyledButton(QStringLiteral("Scan Now"), QStringLiteral("default"));
    m_scanNowBtn->setObjectName(QStringLiteral("ScanNowButton"));
    m_scanNowBtn->setFixedSize(130, UISizes::buttonHeight);
    m_scanNowBtn->setFocusPolicy(Qt::NoFocus);
    m_scanNowBtn->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Primary)
        + QStringLiteral(
        " QPushButton#ScanNowButton {"
        "  min-width: 130px; max-width: 130px;"
        "  min-height: 32px; max-height: 32px;"
        "  padding: 0px 16px;"
        "}"
    ));
    connect(m_scanNowBtn, &QPushButton::clicked, this, &LibrarySettingsTab::onScanNowClicked);
    scanRowLayout->addWidget(m_scanNowBtn);

    m_fullRescanBtn = new StyledButton(QStringLiteral("Full Rescan"), QStringLiteral("default"), scanRow);
    m_fullRescanBtn->setObjectName(QStringLiteral("FullRescanButton"));
    m_fullRescanBtn->setFixedSize(130, UISizes::buttonHeight);
    m_fullRescanBtn->setFocusPolicy(Qt::NoFocus);
    m_fullRescanBtn->setCursor(Qt::PointingHandCursor);
    m_fullRescanBtn->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Destructive)
        + QStringLiteral(
        " QPushButton#FullRescanButton {"
        "  min-width: 130px; max-width: 130px;"
        "  min-height: 32px; max-height: 32px;"
        "  padding: 0px 16px;"
        "}"
    ));
    connect(m_fullRescanBtn, &QPushButton::clicked, this, &LibrarySettingsTab::onFullRescanClicked);
    scanRowLayout->addWidget(m_fullRescanBtn);

    m_scanStatusLabel = new QLabel(QStringLiteral(""), scanRow);
    m_scanStatusLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    scanRowLayout->addWidget(m_scanStatusLabel, 1);

    layout->addWidget(scanRow);

    auto* scanIntervalCombo = new StyledComboBox();
    scanIntervalCombo->addItems({
        QStringLiteral("Manual"),
        QStringLiteral("Every hour"),
        QStringLiteral("Every 6 hours"),
        QStringLiteral("Daily")
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Scan interval"),
        QString(),
        scanIntervalCombo));

    // Ignored file extensions
    auto* ignoreEdit = new QLineEdit();
    ignoreEdit->setText(Settings::instance()->ignoreExtensions().join(QStringLiteral("; ")));
    ignoreEdit->setPlaceholderText(QStringLiteral("cue; log; txt; ..."));
    ignoreEdit->setStyleSheet(
        QStringLiteral("QLineEdit { background: %1; color: %2; border: 1px solid %3; "
                        "border-radius: 6px; padding: 4px 8px; font-size: 12px; }")
            .arg(ThemeManager::instance()->colors().backgroundSecondary,
                 ThemeManager::instance()->colors().foreground,
                 ThemeManager::instance()->colors().border));
    connect(ignoreEdit, &QLineEdit::editingFinished, this, [ignoreEdit]() {
        QStringList exts;
        for (const QString& ext : ignoreEdit->text().split(QRegularExpression(QStringLiteral("[;,\\s]+")),
                                                            Qt::SkipEmptyParts))
            exts.append(ext.trimmed().toLower());
        Settings::instance()->setIgnoreExtensions(exts);
    });

    auto* resetIgnoreBtn = new StyledButton(QStringLiteral("Reset"), QStringLiteral("outline"));
    resetIgnoreBtn->setFixedWidth(70);
    connect(resetIgnoreBtn, &QPushButton::clicked, this, [ignoreEdit]() {
        Settings::instance()->setIgnoreExtensions({});
        ignoreEdit->setText(Settings::instance()->ignoreExtensions().join(QStringLiteral("; ")));
    });

    auto* ignoreRow = new QWidget();
    auto* ignoreRowLayout = new QHBoxLayout(ignoreRow);
    ignoreRowLayout->setContentsMargins(0, 0, 0, 0);
    ignoreRowLayout->setSpacing(8);
    ignoreRowLayout->addWidget(ignoreEdit, 1);
    ignoreRowLayout->addWidget(resetIgnoreBtn);

    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Ignored file extensions"),
        QStringLiteral("Extensions to skip during scan (semicolon-separated)"),
        ignoreRow));

    // ── Section: Organization ──────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Organization")));

    auto* namingPatternCombo = new StyledComboBox();
    namingPatternCombo->addItems({
        QStringLiteral("{artist}/{album}/{track} - {title}"),
        QStringLiteral("{artist} - {album}/{track}. {title}"),
        QStringLiteral("{album}/{track} - {title}")
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Naming Pattern"),
        QString(),
        namingPatternCombo));

    auto* groupCompSwitch = new StyledSwitch();
    groupCompSwitch->setChecked(true);
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Group compilations"),
        QString(),
        groupCompSwitch));

    // ── Section: Auto-Organize ────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Auto-Organize")));

    auto* autoOrgSwitch = new StyledSwitch();
    autoOrgSwitch->setChecked(Settings::instance()->autoOrganizeOnImport());
    connect(autoOrgSwitch, &StyledSwitch::toggled, this, [this, autoOrgSwitch](bool checked) {
        if (checked) {
            bool ok = StyledMessageBox::confirm(window(),
                QStringLiteral("Enable Auto-Organize?"),
                QStringLiteral("Imported files will be renamed and moved to match their metadata. "
                               "This cannot be undone. Continue?"));
            if (!ok) {
                QSignalBlocker blocker(autoOrgSwitch);
                autoOrgSwitch->setChecked(false);
                return;
            }
        }
        Settings::instance()->setAutoOrganizeOnImport(checked);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Auto-organize on import"),
        QStringLiteral("Rename and move files to match metadata"),
        autoOrgSwitch));

    auto* orgPatternCombo = new StyledComboBox();
    orgPatternCombo->setEditable(true);
    orgPatternCombo->addItems({
        QStringLiteral("%artist%/%album%/%track% - %title%"),
        QStringLiteral("%artist% - %album%/%track%. %title%"),
        QStringLiteral("%genre%/%artist%/%album%/%track% - %title%")
    });
    orgPatternCombo->setCurrentText(Settings::instance()->organizePattern());
    connect(orgPatternCombo, &QComboBox::currentTextChanged, this, [](const QString& text) {
        Settings::instance()->setOrganizePattern(text);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Organize pattern"),
        QStringLiteral("Tokens: %artist%, %album%, %title%, %track%, %year%, %genre%"),
        orgPatternCombo));

    // ── Pattern preview example ─────────────────────────────────────
    auto* previewLabel = new QLabel(this);
    previewLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none; padding: 4px 0;")
            .arg(ThemeManager::instance()->colors().accent));
    auto updatePreview = [previewLabel, orgPatternCombo]() {
        QString example = orgPatternCombo->currentText();
        example.replace(QStringLiteral("%artist%"), QStringLiteral("Adele"));
        example.replace(QStringLiteral("%album%"), QStringLiteral("25"));
        example.replace(QStringLiteral("%title%"), QStringLiteral("Hello"));
        example.replace(QStringLiteral("%track%"), QStringLiteral("01"));
        example.replace(QStringLiteral("%year%"), QStringLiteral("2015"));
        example.replace(QStringLiteral("%genre%"), QStringLiteral("Pop"));
        previewLabel->setText(QStringLiteral("Example: %1.flac").arg(example));
    };
    connect(orgPatternCombo, &QComboBox::currentTextChanged, this, updatePreview);
    updatePreview();
    layout->addWidget(previewLabel);

    // ── Section: Library Cleanup ─────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Library Cleanup")));

    auto* cleanupDesc = new QLabel(
        QStringLiteral("Remove duplicate tracks and entries for files that no longer exist."), this);
    cleanupDesc->setWordWrap(true);
    cleanupDesc->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none; padding: 4px 0;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    layout->addWidget(cleanupDesc);

    auto* cleanupBtn = new StyledButton(QStringLiteral("Clean Up Library"),
                                         QStringLiteral("default"));
    cleanupBtn->setFixedHeight(UISizes::buttonHeight);
    cleanupBtn->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Destructive));
    connect(cleanupBtn, &QPushButton::clicked, this, []() {
        LibraryDatabase::instance()->removeDuplicates();
        MusicDataProvider::instance()->reloadFromDatabase();
    });
    layout->addWidget(cleanupBtn);

    // ── Section: Metadata ──────────────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Metadata")));

    auto* metaSwitch = new StyledSwitch();
    metaSwitch->setChecked(Settings::instance()->internetMetadataEnabled());
    connect(metaSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setInternetMetadataEnabled(checked);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Internet Metadata Lookup"),
        QStringLiteral("Automatically fetch artist images, album art, lyrics, and biographies "
                       "from online services. Disabling this prevents all automatic network requests. "
                       "Manual operations (Fix Metadata, Identify by Audio) are not affected."),
        metaSwitch));

    // ── Section: Library Rollback ────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Library Rollback")));

    auto* rollbackDesc = new QLabel(
        QStringLiteral("Restore library data from before the last rescan or metadata rebuild. "
                       "Your music files are never modified."), this);
    rollbackDesc->setWordWrap(true);
    rollbackDesc->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none; padding: 4px 0;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    layout->addWidget(rollbackDesc);

    m_restoreButton = new StyledButton(QStringLiteral("Restore Previous Library Data"),
                                        QStringLiteral("default"));
    m_restoreButton->setFixedHeight(UISizes::buttonHeight);
    m_restoreButton->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Secondary));
    m_restoreButton->setEnabled(LibraryDatabase::instance()->hasBackup());
    connect(m_restoreButton, &QPushButton::clicked, this, [this]() {
        auto* db = LibraryDatabase::instance();
        QDateTime ts = db->backupTimestamp();
        QString timeStr = ts.isValid() ? ts.toString(QStringLiteral("yyyy-MM-dd hh:mm")) : QStringLiteral("unknown");

        if (!StyledMessageBox::confirm(this,
                QStringLiteral("Restore Library Data"),
                QStringLiteral("Restore library data from %1?\n\n"
                               "This will undo the last metadata rebuild or rescan.\n"
                               "Your music files will not be affected.").arg(timeStr)))
            return;

        bool ok = db->restoreFromBackup();
        if (ok) {
            MusicDataProvider::instance()->reloadFromDatabase();
            StyledMessageBox::info(this,
                QStringLiteral("Restored"),
                QStringLiteral("Library data restored successfully."));
            m_restoreButton->setEnabled(db->hasBackup());
        } else {
            StyledMessageBox::warning(this,
                QStringLiteral("Restore Failed"),
                QStringLiteral("Could not restore from backup."));
        }
    });
    layout->addWidget(m_restoreButton);

    // Update restore button when database changes
    connect(LibraryDatabase::instance(), &LibraryDatabase::databaseChanged,
            this, [this]() {
        if (m_restoreButton)
            m_restoreButton->setEnabled(LibraryDatabase::instance()->hasBackup());
    });

    layout->addStretch();

    scrollArea->setWidget(content);
    outerLayout->addWidget(scrollArea);

    // ── Scanner signal connections ──────────────────────────────────
    connect(LibraryScanner::instance(), &LibraryScanner::scanProgress,
            this, &LibrarySettingsTab::onScanProgress);
    connect(LibraryScanner::instance(), &LibraryScanner::scanFinished,
            this, &LibrarySettingsTab::onScanFinished);
}

// ═════════════════════════════════════════════════════════════════════
//  rebuildFolderList
// ═════════════════════════════════════════════════════════════════════

void LibrarySettingsTab::rebuildFolderList()
{
    // Clear existing folder widgets
    while (m_foldersLayout->count() > 0) {
        QLayoutItem* item = m_foldersLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    const QStringList folders = Settings::instance()->libraryFolders();

    if (folders.isEmpty()) {
        auto* emptyLabel = new QLabel(
            QStringLiteral("No folders added yet. Click \"Add Folder\" to get started."),
            m_foldersContainer);
        emptyLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 13px; border: none; padding: 8px 0;")
                .arg(ThemeManager::instance()->colors().foregroundMuted));
        m_foldersLayout->addWidget(emptyLabel);
        return;
    }

    for (const QString& folder : folders) {
        auto* folderWidget = new QWidget(m_foldersContainer);
        auto* folderLayout = new QHBoxLayout(folderWidget);
        folderLayout->setContentsMargins(0, 4, 0, 4);
        folderLayout->setSpacing(8);

        auto* folderLabel = new QLabel(folder, folderWidget);
        folderLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 13px; border: none;")
                .arg(ThemeManager::instance()->colors().foreground));
        folderLayout->addWidget(folderLabel, 1);

        auto* removeBtn = new StyledButton(QStringLiteral(""),
                                            QStringLiteral("ghost"),
                                            folderWidget);
        removeBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/trash-2.svg")));
        removeBtn->setFixedSize(UISizes::smallButtonSize, UISizes::smallButtonSize);
        removeBtn->setIconSize(QSize(UISizes::toggleIconSize, UISizes::toggleIconSize));

        QString folderPath = folder; // capture for lambda
        connect(removeBtn, &QPushButton::clicked, this, [this, folderPath]() {
            onRemoveFolderClicked(folderPath);
        });
        folderLayout->addWidget(removeBtn);

        m_foldersLayout->addWidget(folderWidget);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  Folder management slots
// ═════════════════════════════════════════════════════════════════════

void LibrarySettingsTab::onAddFolderClicked()
{
    QString folder = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select Music Folder"),
        QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!folder.isEmpty()) {
#ifdef Q_OS_MACOS
        BookmarkManager::instance()->saveBookmark(folder);
#endif
        Settings::instance()->addLibraryFolder(folder);
        rebuildFolderList();

        // Auto-scan all folders including newly added one
        QStringList folders = Settings::instance()->libraryFolders();
        LibraryScanner::instance()->scanFolders(folders);
        qDebug() << "[Settings] Folder added — auto-scan triggered:" << folder;
    }
}

void LibrarySettingsTab::onRemoveFolderClicked(const QString& folder)
{
#ifdef Q_OS_MACOS
    BookmarkManager::instance()->removeBookmark(folder);
#endif
    Settings::instance()->removeLibraryFolder(folder);
    rebuildFolderList();
}

void LibrarySettingsTab::onScanNowClicked()
{
    QStringList folders = Settings::instance()->libraryFolders();
    if (folders.isEmpty()) {
        m_scanStatusLabel->setText(QStringLiteral("No folders to scan. Add a folder first."));
        return;
    }

    m_scanNowBtn->setEnabled(false);
    m_fullRescanBtn->setEnabled(false);
    m_scanStatusLabel->setText(QStringLiteral("Scanning..."));

    LibraryScanner::instance()->scanFolders(folders);
}

void LibrarySettingsTab::onFullRescanClicked()
{
    QStringList folders = Settings::instance()->libraryFolders();
    if (folders.isEmpty()) {
        m_scanStatusLabel->setText(QStringLiteral("No folders to scan. Add a folder first."));
        return;
    }

    if (!StyledMessageBox::confirm(this,
            QStringLiteral("Full Rescan"),
            QStringLiteral("This will clear your library and rescan all files.\nPlaylists will be preserved.\n\nContinue?")))
        return;

    m_scanNowBtn->setEnabled(false);
    m_fullRescanBtn->setEnabled(false);
    m_scanStatusLabel->setText(QStringLiteral("Backing up and rescanning..."));

    // Auto-backup before destructive operation
    auto* db = LibraryDatabase::instance();
    db->createBackup();
    if (m_restoreButton)
        m_restoreButton->setEnabled(db->hasBackup());
    db->clearAllData(true);  // preserves playlists

    LibraryScanner::instance()->scanFolders(folders);
}

void LibrarySettingsTab::onScanProgress(int current, int total)
{
    m_scanStatusLabel->setText(
        QStringLiteral("Scanning... %1 / %2 files").arg(current).arg(total));
}

void LibrarySettingsTab::onScanFinished(int tracksFound)
{
    m_scanNowBtn->setEnabled(true);
    m_fullRescanBtn->setEnabled(true);
    m_scanStatusLabel->setText(
        QStringLiteral("Scan complete. %1 tracks found.").arg(tracksFound));
    // reloadFromDatabase() already triggered by rebuildAlbumsAndArtists → databaseChanged signal
}

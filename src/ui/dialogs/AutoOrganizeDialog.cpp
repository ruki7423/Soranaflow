#include "AutoOrganizeDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include "StyledMessageBox.h"
#include <QHeaderView>
#include <QDebug>

#include "../../core/ThemeManager.h"

// ═══════════════════════════════════════════════════════════════════════
//  Constructor
// ═══════════════════════════════════════════════════════════════════════

AutoOrganizeDialog::AutoOrganizeDialog(const QStringList& filePaths, QWidget* parent)
    : QDialog(parent)
    , m_filePaths(filePaths)
{
    setupUi();
    refreshPreview();
}

// ═══════════════════════════════════════════════════════════════════════
//  setupUi
// ═══════════════════════════════════════════════════════════════════════

void AutoOrganizeDialog::setupUi()
{
    setWindowTitle(tr("Auto-Organize Files"));
    setMinimumSize(700, 500);
    resize(750, 550);

    auto* tm = ThemeManager::instance();
    auto c = tm->colors();
    setStyleSheet(QStringLiteral("QDialog { background-color: %1; }"
                          "QLabel { color: %2; }"
                          "QLineEdit { background-color: %3; color: %2;"
                          " border: 1px solid %4; border-radius: 4px; padding: 6px; }"
                          "QTreeWidget { background-color: %3; color: %2;"
                          " border: 1px solid %4; border-radius: 4px; }"
                          "QHeaderView::section { background-color: %3; color: %2;"
                          " border: none; padding: 6px; }")
        .arg(c.backgroundElevated, c.foreground, c.backgroundTertiary, c.border));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(UISizes::spacingMD);
    mainLayout->setContentsMargins(20, 20,
                                    20, 20);

    // ── Pattern ──────────────────────────────────────────────────
    auto* patternLayout = new QFormLayout();

    m_patternEdit = new QLineEdit(m_organizer.pattern(), this);
    m_patternEdit->setPlaceholderText(tr("e.g. %artist%/%album%/%track% - %title%"));
    connect(m_patternEdit, &QLineEdit::textChanged, this, &AutoOrganizeDialog::onPatternChanged);
    patternLayout->addRow(tr("Pattern:"), m_patternEdit);

    auto* tokensLabel = new QLabel(
        tr("Tokens: %artist%, %album%, %title%, %track%, %year%, %genre%"), this);
    tokensLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(c.foregroundMuted));
    patternLayout->addRow(QString(), tokensLabel);

    mainLayout->addLayout(patternLayout);

    // ── Destination ──────────────────────────────────────────────
    auto* destLayout = new QHBoxLayout();
    m_destEdit = new QLineEdit(this);
    m_destEdit->setPlaceholderText(tr("Destination folder..."));
    connect(m_destEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_organizer.setDestinationRoot(text);
        refreshPreview();
    });
    destLayout->addWidget(m_destEdit);

    QString btnStyle = tm->buttonStyle(ButtonVariant::Secondary);

    auto* browseBtn = new QPushButton(tr("Browse..."), this);
    browseBtn->setStyleSheet(btnStyle);
    connect(browseBtn, &QPushButton::clicked, this, &AutoOrganizeDialog::onBrowseDestination);
    destLayout->addWidget(browseBtn);

    mainLayout->addLayout(destLayout);

    // ── Preview tree ─────────────────────────────────────────────
    m_previewTree = new QTreeWidget(this);
    m_previewTree->setHeaderLabels({tr("Source"), tr("Destination")});
    m_previewTree->header()->setStretchLastSection(true);
    m_previewTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_previewTree->setRootIsDecorated(false);
    m_previewTree->setAlternatingRowColors(true);
    mainLayout->addWidget(m_previewTree, 1);

    // ── Status ───────────────────────────────────────────────────
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted));
    mainLayout->addWidget(m_statusLabel);

    // ── Buttons ──────────────────────────────────────────────────
    auto* buttonLayout = new QHBoxLayout();

    m_undoBtn = new QPushButton(tr("Undo Last"), this);
    m_undoBtn->setStyleSheet(btnStyle);
    m_undoBtn->setEnabled(false);
    connect(m_undoBtn, &QPushButton::clicked, this, &AutoOrganizeDialog::onUndo);
    buttonLayout->addWidget(m_undoBtn);

    buttonLayout->addStretch();

    auto* cancelBtn = new QPushButton(tr("Cancel"), this);
    cancelBtn->setStyleSheet(btnStyle);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(cancelBtn);

    m_executeBtn = new QPushButton(tr("Organize"), this);
    m_executeBtn->setStyleSheet(tm->buttonStyle(ButtonVariant::Primary));
    connect(m_executeBtn, &QPushButton::clicked, this, &AutoOrganizeDialog::onExecute);
    buttonLayout->addWidget(m_executeBtn);

    mainLayout->addLayout(buttonLayout);
}

// ═══════════════════════════════════════════════════════════════════════
//  Slots
// ═══════════════════════════════════════════════════════════════════════

void AutoOrganizeDialog::onPatternChanged()
{
    m_organizer.setPattern(m_patternEdit->text());
    refreshPreview();
}

void AutoOrganizeDialog::onBrowseDestination()
{
    QString dir = QFileDialog::getExistingDirectory(this, tr("Select Destination Folder"));
    if (!dir.isEmpty()) {
        m_destEdit->setText(dir);
    }
}

void AutoOrganizeDialog::onExecute()
{
    if (m_previewActions.isEmpty()) {
        StyledMessageBox::info(this, tr("Nothing to Do"),
            tr("No files need to be moved."));
        return;
    }

    if (m_organizer.execute(m_previewActions)) {
        m_undoBtn->setEnabled(true);
        m_statusLabel->setText(tr("Organized %1 files successfully.").arg(m_previewActions.size()));
        refreshPreview(); // Should now show empty
    } else {
        StyledMessageBox::warning(this, tr("Error"),
            tr("Some files could not be moved. Check the console for details."));
    }
}

void AutoOrganizeDialog::onUndo()
{
    if (m_organizer.undo()) {
        m_undoBtn->setEnabled(false);
        m_statusLabel->setText(tr("Undo complete."));
        refreshPreview();
    } else {
        StyledMessageBox::warning(this, tr("Error"),
            tr("Could not undo the last operation."));
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  refreshPreview
// ═══════════════════════════════════════════════════════════════════════

void AutoOrganizeDialog::refreshPreview()
{
    m_previewTree->clear();

    if (m_organizer.destinationRoot().isEmpty()) {
        m_statusLabel->setText(tr("Select a destination folder."));
        m_executeBtn->setEnabled(false);
        return;
    }

    m_previewActions = m_organizer.preview(m_filePaths);

    for (const auto& action : m_previewActions) {
        auto* item = new QTreeWidgetItem(m_previewTree);
        item->setText(0, QFileInfo(action.sourcePath).fileName());
        item->setText(1, action.destPath);
        item->setToolTip(0, action.sourcePath);
        item->setToolTip(1, action.destPath);
    }

    m_statusLabel->setText(tr("%1 files to organize.").arg(m_previewActions.size()));
    m_executeBtn->setEnabled(!m_previewActions.isEmpty());
}

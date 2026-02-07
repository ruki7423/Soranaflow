#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>

class NewPlaylistDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NewPlaylistDialog(QWidget* parent = nullptr);
    QString playlistName() const;

private slots:
    void onTextChanged(const QString& text);

private:
    void setupUI();
    void applyTheme();

    QLineEdit*   m_nameEdit;
    QPushButton* m_cancelButton;
    QPushButton* m_okButton;
    QLabel*      m_titleLabel;
    QLabel*      m_nameLabel;
};

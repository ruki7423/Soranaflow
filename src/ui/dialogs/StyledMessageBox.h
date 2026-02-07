#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QHBoxLayout;
class StyledButton;

class StyledMessageBox : public QDialog
{
    Q_OBJECT

public:
    enum class Icon {
        None,
        Question,
        Warning,
        Error,
        Info
    };

    enum class ButtonType {
        Ok,
        Cancel,
        Yes,
        No,
        Delete,
        Save,
        Discard
    };

    explicit StyledMessageBox(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setMessage(const QString& message);
    void setIcon(Icon icon);
    void addButton(ButtonType type, bool isPrimary = false);

    ButtonType clickedButton() const { return m_clickedButton; }

    // Convenience static methods
    static bool confirm(QWidget* parent, const QString& title, const QString& message);
    static bool confirmDelete(QWidget* parent, const QString& itemName);
    static void info(QWidget* parent, const QString& title, const QString& message);
    static void warning(QWidget* parent, const QString& title, const QString& message);
    static void error(QWidget* parent, const QString& title, const QString& message);

private:
    void setupUi();
    QString buttonText(ButtonType type) const;

    QLabel* m_iconLabel = nullptr;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_messageLabel = nullptr;
    QWidget* m_buttonContainer = nullptr;
    QHBoxLayout* m_buttonLayout = nullptr;
    ButtonType m_clickedButton = ButtonType::Cancel;
};

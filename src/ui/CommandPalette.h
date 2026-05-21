#pragma once

#include <QString>
#include <QVector>
#include <QWidget>
#include <functional>

class QFrame;
class QLineEdit;
class QListWidget;

// ⌘K-style command palette: a dim full-window overlay with a centered search
// card over a fuzzy-filtered action list. Self-contained — callers register
// commands with callbacks; the palette handles filtering, keyboard nav, and
// invocation. Mirrors the command pill / palette in the MalloyStudio design.
class CommandPalette : public QWidget {
    Q_OBJECT
public:
    struct Command {
        QString id;
        QString title;
        QString subtitle;
        QString icon;
        std::function<void()> run;
    };

    explicit CommandPalette(QWidget* parent = nullptr);

    void setCommands(QVector<Command> commands);
    void openPalette();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void refilter(const QString& query);
    void runCurrent();
    void moveSelection(int delta);

    QFrame* m_card = nullptr;
    QLineEdit* m_search = nullptr;
    QListWidget* m_list = nullptr;
    QVector<Command> m_commands;
};

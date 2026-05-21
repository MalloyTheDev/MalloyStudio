#pragma once

#include <QWidget>

// AI Lab preview screen (secondary.jsx AILab): a planned-features showcase —
// hero with a Preview badge, a grid of "Soon" feature cards, and a note about
// the local-first stack. Purely informational in this build.
class AILabWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit AILabWorkspace(QWidget* parent = nullptr);
};

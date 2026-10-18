#pragma once

#include <QAction>
#include <QDialog>
#include <QIcon>
#include <QList>
#include <QMessageBox>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>

class QEventLoop;
class QTimer;
class GlCanvas;
class GlLabel;
class GlButton;
class GlPanel;
class GlTextInput;
class GlDropdown;

// Motiva-styled popup menu with the QMenu surface the app already uses
// (QAction based, submenus, separators, exec()). Painted with the active
// theme's GlUi::Style; each popup is a small borderless window.
class GlMenu : public QWidget {
    Q_OBJECT
public:
    explicit GlMenu(QWidget* parent = nullptr);
    ~GlMenu() override;

    QAction* addAction(const QString& text);
    QAction* addAction(const QIcon& icon, const QString& text);
    template <class Obj, class Fn>
    QAction* addAction(const QString& text, const Obj* receiver, Fn slot) {
        QAction* a = addAction(text);
        connect(a, &QAction::triggered, receiver, slot);
        return a;
    }
    template <class Obj, class Fn>
    QAction* addAction(const QIcon& icon, const QString& text, const Obj* receiver, Fn slot) {
        QAction* a = addAction(icon, text);
        connect(a, &QAction::triggered, receiver, slot);
        return a;
    }
    QAction* addAction(QAction* existing); // shares an action owned elsewhere
    QAction* addSeparator();
    GlMenu* addMenu(const QString& title);
    void setToolTipsVisible(bool) {}
    // On a submenu: enables/disables the parent menu row that opens it.
    void setEnabled(bool on);
    bool isEmpty() const { return m_entries.isEmpty(); }

    // Blocks until the menu closes; triggers and returns the chosen action
    // (nullptr when dismissed), like QMenu::exec.
    QAction* exec(const QPoint& globalPos);
    // Non-blocking (tray icon): the chosen action is triggered when picked.
    void popup(const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    struct Entry {
        QAction* action = nullptr;
        GlMenu* sub = nullptr;
    };
    GlMenu(QWidget* parent, GlMenu* parentMenu);
    GlMenu* root();
    QSize computeSize() const;
    QRect rowRect(int i) const;
    int rowAt(const QPoint& p) const;
    bool selectable(int i) const;
    void setHover(int i, bool openSub);
    void openSub(int i);
    void closeSub();
    void choose(int i);
    void step(int dir);

    QList<Entry> m_entries;
    GlMenu* m_parentMenu = nullptr;
    GlMenu* m_openSub = nullptr;
    int m_hover = -1;
    QAction* m_result = nullptr;
    QEventLoop* m_loop = nullptr;
    QTimer* m_subTimer = nullptr;
    bool m_hasCheck = false;
};

// Motiva-styled message and input boxes with QMessageBox / QInputDialog
// shaped entry points, drawn on the GL canvas like the other windows.
class GlMessageBox : public QDialog {
    Q_OBJECT
public:
    enum class Icon { None, Information, Question, Warning, Critical };
    // Opaque handle for a button added with addButton().
    class Button : public QObject {
    public:
        using QObject::QObject;
        int id = 0;
    };
    GlMessageBox(Icon icon, const QString& title, const QString& text, QWidget* parent = nullptr);
    // QMessageBox-shaped constructors used by existing call sites.
    GlMessageBox(QMessageBox::Icon icon, const QString& title, const QString& text, QWidget* parent = nullptr);
    GlMessageBox(QMessageBox::Icon icon, const QString& title, const QString& text,
                 QMessageBox::StandardButtons buttons, QWidget* parent = nullptr);
    explicit GlMessageBox(QWidget* parent);
    void setIcon(QMessageBox::Icon icon);
    void setText(const QString& text);
    void setInformativeText(const QString& info);
    void setEscapeButton(Button* b) { m_escape = b; }

    Button* addButton(const QString& text, QMessageBox::ButtonRole role);
    Button* addButton(QMessageBox::StandardButton button);
    void setDefaultButton(Button* b) { m_default = b; }
    Button* clickedButton() const { return m_clicked; }
    QMessageBox::StandardButton clickedStandardButton() const { return m_clickedStd; }

    static QMessageBox::StandardButton question(QWidget* parent, const QString& title, const QString& text,
                                                QMessageBox::StandardButtons buttons = QMessageBox::Yes | QMessageBox::No,
                                                QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
    static QMessageBox::StandardButton information(QWidget* parent, const QString& title, const QString& text,
                                                   QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                                   QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
    static QMessageBox::StandardButton warning(QWidget* parent, const QString& title, const QString& text,
                                               QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                               QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
    static QMessageBox::StandardButton critical(QWidget* parent, const QString& title, const QString& text,
                                                QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                                QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

    static QString getText(QWidget* parent, const QString& title, const QString& label, const QString& text,
                           bool* ok);
    // Same, with a custom confirm-button caption and a placeholder.
    static QString getText(QWidget* parent, const QString& title, const QString& label, const QString& text,
                           const QString& okText, const QString& placeholder, bool* ok);
    // QInputDialog-shaped overloads (echo mode / editable are accepted and ignored).
    static QString getText(QWidget* parent, const QString& title, const QString& label, int echoMode,
                           const QString& text, bool* ok) {
        Q_UNUSED(echoMode);
        return getText(parent, title, label, text, ok);
    }
    static QString getItem(QWidget* parent, const QString& title, const QString& label, const QStringList& items,
                           int current, bool editable, bool* ok) {
        Q_UNUSED(editable);
        return getItem(parent, title, label, items, current, ok);
    }
    static QString getItem(QWidget* parent, const QString& title, const QString& label, const QStringList& items,
                           int current, bool* ok);

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void showEvent(QShowEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    struct Slot {
        Button* handle;
        GlButton* item;
        QMessageBox::ButtonRole role;
        QMessageBox::StandardButton std;
    };
    void layoutGl(const QSizeF& size);
    void finish(Slot& s);
    QSize wantedSize() const;
    void buildChrome(const QString& title, const QString& text);

    GlCanvas* m_canvas = nullptr;
    GlPanel* m_titlePanel = nullptr;
    GlPanel* m_bodyPanel = nullptr;
    GlLabel* m_titleLabel = nullptr;
    GlLabel* m_textLabel = nullptr;
    QString m_text;
    Icon m_icon;
    QList<Slot> m_slots;
    Button* m_default = nullptr;
    Button* m_escape = nullptr;
    Button* m_clicked = nullptr;
    QMessageBox::StandardButton m_clickedStd = QMessageBox::NoButton;
    int m_nextId = 1;
    // Optional input field (getText / getItem).
    GlTextInput* m_input = nullptr;
    GlDropdown* m_choice = nullptr;
    GlLabel* m_inputLabel = nullptr;
    friend class GlInputHelper;
};

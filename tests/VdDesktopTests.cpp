#include "VdDesktopModel.h"

#include <QtTest>

using namespace VdDesktop;

class VdDesktopTests : public QObject {
    Q_OBJECT
    const QSizeF area{1920, 1080};

    static QPointF centerOf(const QRectF& r) { return r.center(); }

private slots:
    void startButtonTogglesMenu() {
        Model m;
        m.setArea(area);
        QVERIFY(!m.startOpen());
        QVERIFY(m.press(centerOf(slotRect(area, 0))));
        QVERIFY(m.startOpen());
        QVERIFY(m.press(centerOf(slotRect(area, 0))));
        QVERIFY(!m.startOpen());
    }
    void clickInsideMenuKeepsItOpenClickAwayClosesIt() {
        Model m;
        m.setArea(area);
        m.press(centerOf(slotRect(area, 0)));
        QVERIFY(m.press(centerOf(startMenuRect(area))));
        QVERIFY(m.startOpen());
        QVERIFY(!m.press(QPointF(5, 5))); // empty desktop: not consumed, but closes the menu
        QVERIFY(!m.startOpen());
    }
    void taskbarSlotsOpenAndCloseWindows() {
        Model m;
        m.setArea(area);
        QVERIFY(!m.window(0).open);
        m.press(centerOf(slotRect(area, 1)));
        QVERIFY(m.window(0).open);
        m.press(centerOf(slotRect(area, 1)));
        QVERIFY(!m.window(0).open);
    }
    void closeButtonClosesWindow() {
        Model m;
        m.setArea(area);
        m.press(centerOf(slotRect(area, 2)));
        QVERIFY(m.window(1).open);
        QVERIFY(m.press(centerOf(closeButtonRect(m.window(1).rect))));
        QVERIFY(!m.window(1).open);
    }
    void clickingWindowRaisesIt() {
        Model m;
        m.setArea(area);
        m.press(centerOf(slotRect(area, 1)));
        m.press(centerOf(slotRect(area, 2)));
        QCOMPARE(m.zOrder().last(), 1); // opened last -> on top
        // A point inside window 0's body that window 1 does not cover.
        const QRectF r0 = m.window(0).rect;
        const QPointF p(r0.left() + 5, r0.bottom() - 5);
        QVERIFY(!m.window(1).rect.contains(p));
        QVERIFY(m.press(p));
        QCOMPARE(m.zOrder().last(), 0);
    }
    void titleBarDragMovesAndClampsWindow() {
        Model m;
        m.setArea(area);
        m.press(centerOf(slotRect(area, 1)));
        const QRectF before = m.window(0).rect;
        const QPointF grab = titleBarRect(before).center();
        QVERIFY(m.press(grab));
        QVERIFY(m.isDragging());
        m.dragTo(grab + QPointF(50, 40));
        QCOMPARE(m.window(0).rect.topLeft(), before.topLeft() + QPointF(50, 40));
        m.dragTo(QPointF(-5000, -5000));
        QCOMPARE(m.window(0).rect.topLeft(), QPointF(0, 0));
        m.dragTo(QPointF(9000, 9000));
        QVERIFY(m.window(0).rect.right() <= area.width());
        QVERIFY(m.window(0).rect.bottom() <= area.height() - taskbarHeight(area));
        m.release();
        QVERIFY(!m.isDragging());
    }
    void windowBodyClickDoesNotDrag() {
        Model m;
        m.setArea(area);
        m.press(centerOf(slotRect(area, 1)));
        QVERIFY(m.press(centerOf(m.window(0).rect)));
        QVERIFY(!m.isDragging());
    }
    void shrinkingAreaKeepsWindowsOnScreen() {
        Model m;
        m.setArea(area);
        m.setArea(QSizeF(800, 600));
        for (int i = 0; i < kWindowCount; ++i) {
            QVERIFY(m.window(i).rect.right() <= 800.0);
            QVERIFY(m.window(i).rect.bottom() <= 600.0 - taskbarHeight(QSizeF(800, 600)));
        }
    }
    void pressBeforeAreaIsSetIsIgnored() {
        Model m;
        QVERIFY(!m.press(QPointF(10, 10)));
    }
    void pausePolicy() {
        QVERIFY(!shouldPausePlayback(true, false, true));  // AC power: play
        QVERIFY(shouldPausePlayback(true, true, true));    // battery + policy: pause
        QVERIFY(!shouldPausePlayback(false, true, true));  // battery but policy allows: play
        QVERIFY(shouldPausePlayback(false, false, false)); // nothing showing: pause
    }
    void screenSyncPlan() {
        const ScreenPlan same = planScreenSync({"A", "B"}, {"A", "B"});
        QVERIFY(same.add.isEmpty() && same.remove.isEmpty());
        const ScreenPlan plug = planScreenSync({"A"}, {"A", "B"});
        QCOMPARE(plug.add, QStringList{"B"});
        QVERIFY(plug.remove.isEmpty());
        const ScreenPlan unplug = planScreenSync({"A", "B"}, {"A"});
        QCOMPARE(unplug.remove, QStringList{"B"});
        QVERIFY(unplug.add.isEmpty());
        const ScreenPlan swap = planScreenSync({"A", "B"}, {"B", "C"});
        QCOMPARE(swap.add, QStringList{"C"});
        QCOMPARE(swap.remove, QStringList{"A"});
    }
};

QTEST_APPLESS_MAIN(VdDesktopTests)
#include "VdDesktopTests.moc"

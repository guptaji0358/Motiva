#include "WebMediaName.h"

#include <QtTest>

class WebMediaNameTests : public QObject {
    Q_OBJECT
private slots:
    void sanitized_data() {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        auto row = [](const char* tag, const QString& in, const QString& out) { QTest::newRow(tag) << in << out; };

        row("plain", "photo.jpg", "photo.jpg");
        row("folders", "a/b\\c.png", "c.png");
        row("forbidden chars", "a<b>c?.gif", "a_b_c_.gif");
        row("trailing dots and spaces", "  clip.mp4. . ", "clip.mp4");
        row("space then dot", "clip .", "clip");
        row("only dots", "...", "");
        row("empty", "", "");

        row("device with extension", "CON.mp4", "_CON.mp4");
        row("device without extension", "nul", "_nul");
        row("device, space before dot", "com1 .gif", "_com1 .gif");
        row("device, trailing dot", "AUX.", "_AUX");
        row("device, two extensions", "lpt9.tar.png", "_lpt9.tar.png");
        row("superscript digit", QString::fromUtf8("COM\xC2\xB9.png"), QString::fromUtf8("_COM\xC2\xB9.png"));
        row("console input", "conin$.jpg", "_conin$.jpg");
        row("not a device: prefix", "console.mp4", "console.mp4");
        row("not a device: com0", "com0.mp4", "com0.mp4");
        row("not a device: com10", "com10.mp4", "com10.mp4");

        const QString a(200, QLatin1Char('a'));
        row("exactly the limit", a.left(116) + ".jpg", a.left(116) + ".jpg");
        row("long name keeps extension", a + ".jpg", a.left(116) + ".jpg");
        row("long name, no extension", a, a.left(120));
        row("long extension is not preserved", "x." + a, "x." + a.left(118));
        row("cut without extension is trimmed", a.left(119) + " " + a, a.left(119));

        // U+1F600 is two UTF-16 units; the cut at 116 would fall between them.
        const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
        row("surrogate pair, extension", a.left(115) + emoji + a + ".jpg", a.left(115) + ".jpg");
        row("surrogate pair, no extension", a.left(119) + emoji, a.left(119));
        row("surrogate pair fits", a.left(114) + emoji + a + ".jpg", a.left(114) + emoji + ".jpg");

        // A cut that leaves a device name, and the "_" must not exceed the limit.
        row("device after cut", "con" + QString(130, QLatin1Char(' ')) + "x.jpg",
            "_con" + QString(112, QLatin1Char(' ')) + ".jpg");
        row("long device name", "con." + a + ".jpg", "_con." + a.left(111) + ".jpg");
    }

    void sanitized() {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        const QString result = WebMediaName::sanitized(input);
        QCOMPARE(result, expected);
        QVERIFY(result.size() <= WebMediaName::kMaxLength);
    }
};

QTEST_APPLESS_MAIN(WebMediaNameTests)
#include "WebMediaNameTests.moc"

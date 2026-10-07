#include "window.h"
#include "appbrowser.h"
#include <QApplication>
#include <QFile>
#include <QBuffer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QImage>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <QtEndian>

class GuiTest : public QObject {
    Q_OBJECT
private slots:
    void iconViewsAndSelection() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QImage image(64, 64, QImage::Format_ARGB32); image.fill(Qt::green);
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        QByteArray icns = "icns";
        auto appendLength = [](QByteArray &data, quint32 length) { quint32 big = qToBigEndian(length); data.append(reinterpret_cast<const char *>(&big), 4); };
        appendLength(icns, quint32(16 + png.size())); icns += "icp6"; appendLength(icns, quint32(8 + png.size())); icns += png;
        QString iconPath = temporary.path() + "/App.icns"; QFile file(iconPath); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(icns); file.close();
        AppBrowser browser; browser.resize(500, 350);
        browser.showPreviews({{"Applications/One.app", "Friendly One", iconPath, 1250000}, {"Applications/Two.app", "Two", iconPath, 2000}, {"Applications/Three.app", "Three", {}, 3000}});
        browser.show(); QTest::qWait(30);
        QCOMPARE(browser.item(0)->text(), "Friendly One");
        QCOMPARE(browser.item(0)->icon().pixmap(64, 64).toImage().pixelColor(32, 32), QColor(Qt::green));
        QVERIFY(browser.item(0)->toolTip().contains("MB")); QCOMPARE(browser.iconSize(), QSize(32, 32));
        QTest::mouseClick(browser.viewport(), Qt::LeftButton, Qt::NoModifier, browser.visualItemRect(browser.item(0)).center());
        QTest::keyClick(&browser, Qt::Key_A, Qt::ControlModifier); QCOMPARE(browser.selectedBundles().size(), 3);
        QTest::mouseClick(browser.viewport(), Qt::LeftButton, Qt::NoModifier, browser.visualItemRect(browser.item(0)).center());
        QTest::mouseClick(browser.viewport(), Qt::LeftButton, Qt::ShiftModifier, browser.visualItemRect(browser.item(2)).center()); QCOMPARE(browser.selectedBundles().size(), 3);
        QTest::mouseClick(browser.viewport(), Qt::LeftButton, Qt::ControlModifier, browser.visualItemRect(browser.item(1)).center()); QCOMPARE(browser.selectedBundles().size(), 2);
        browser.setGridView(true); QCOMPARE(browser.viewMode(), QListView::IconMode); QCOMPARE(browser.iconSize(), QSize(64, 64)); QCOMPARE(browser.selectedBundles().size(), 2);
        browser.setGridView(false); QCOMPARE(browser.viewMode(), QListView::ListMode); QCOMPARE(browser.selectedBundles().size(), 2);
    }
    void importDiagnoseRetry() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
        QString volume = temporary.path() + "/volume", prefix = temporary.path() + "/prefix";
        QString app = volume + "/System/Applications/Test.app";
        QVERIFY(QDir().mkpath(app + "/Contents/MacOS"));
        QVERIFY(QDir().mkpath(volume + "/usr/lib"));
        QVERIFY(QDir().mkpath(prefix));
        QFile plist(app + "/Contents/Info.plist"); QVERIFY(plist.open(QIODevice::WriteOnly));
        plist.write("<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleExecutable</key><string>Test</string></dict></plist>"); plist.close();
        QFile executable(app + "/Contents/MacOS/Test"); QVERIFY(executable.open(QIODevice::WriteOnly)); executable.write("fixture"); executable.close();
        QFile library(volume + "/usr/lib/libExample.dylib"); QVERIFY(library.open(QIODevice::WriteOnly)); library.write("fixture"); library.close();
        QString fakePath = temporary.path() + "/fake-darling";
        QFile fake(fakePath); QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write("#!/bin/sh\nif [ -f \"$DPREFIX/usr/lib/libExample.dylib\" ]; then echo launched; exit 0; fi\n"
                   "printf 'Symbol not found: _Example\\n  Referenced from: /Applications/Test.app/Contents/MacOS/Test\\n  Expected in: /usr/lib/libExample.dylib\\n'\nexit 1\n");
        fake.close(); QVERIFY(QFile::setPermissions(fakePath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        Window window; window.show();
        window.findChild<QLineEdit *>("volumeField")->setText(volume);
        window.findChild<QLineEdit *>("prefixField")->setText(prefix);
        window.findChild<QLineEdit *>("darlingField")->setText(fakePath);
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) box->accept();
        });
        dismiss.start(20);
        window.findChild<QPushButton *>("Scan volume")->click();
        auto *available = window.findChild<QListWidget *>("availableApps");
        QTRY_COMPARE_WITH_TIMEOUT(available->count(), 1, 5000);
        available->item(0)->setSelected(true);
        auto *apps = window.findChild<QTableWidget *>("importedApps");
        QMimeData mime; mime.setData("application/x-darling-app-bundles", QJsonDocument(QJsonArray{"System/Applications/Test.app"}).toJson());
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(apps->viewport(), &enter); QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(apps->viewport(), &drop); QVERIFY(drop.isAccepted());
        QTRY_COMPARE_WITH_TIMEOUT(apps->rowCount(), 1, 5000);
        QCOMPARE(apps->item(0, 1)->text(), "Applications/Test.app");
        apps->selectRow(0);
        window.findChild<QPushButton *>("Launch selected")->click();
        QTRY_VERIFY_WITH_TIMEOUT(apps->item(0, 2)->text().contains("Missing _Example"), 5000);
        window.findChild<QPushButton *>("Import needed library and retry")->click();
        QTRY_COMPARE_WITH_TIMEOUT(apps->item(0, 2)->text(), QString("Exited successfully"), 5000);
        QVERIFY(QFileInfo::exists(prefix + "/usr/lib/libExample.dylib"));
        auto catalog = LauncherCore::loadCatalog(prefix);
        QCOMPARE(catalog.value("apps").toArray().first().toObject().value("chain").toArray().size(), 1);
    }
};
QTEST_MAIN(GuiTest)
#include "gui_test.moc"

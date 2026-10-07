#include "window.h"
#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

class GuiTest : public QObject {
    Q_OBJECT
private slots:
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
        QCOMPARE(available->count(), 1);
        available->item(0)->setSelected(true);
        window.findChild<QPushButton *>("Import selected apps")->click();
        auto *apps = window.findChild<QTableWidget *>("importedApps");
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

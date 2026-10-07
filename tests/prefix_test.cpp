#include "prefixbuilder.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QJsonObject readJson(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return QJsonDocument::fromJson(file.readAll()).object(); }
}
class PrefixTest : public QObject {
    Q_OBJECT
private slots:
    void preserveDiscoveryInputs() {
        QJsonObject discovery{{"schema", 1}, {"owner", "VibeDarling"}, {"complete", true},
            {"repos", QJsonArray{QJsonObject{{"repo", "darling"}, {"branch", "master"}, {"base", QString(40, 'a')}, {"prs", QJsonArray{QJsonObject{{"number", 7}}}}}}}};
        QJsonObject selected; QString error;
        QVERIFY(LauncherPrefix::selectInputs(discovery, false, &selected, &error));
        QVERIFY(selected.value("repos").toArray().first().toObject().value("prs").toArray().isEmpty());
        QCOMPARE(discovery.value("repos").toArray().first().toObject().value("prs").toArray().size(), 1);
        QVERIFY(LauncherPrefix::selectInputs(discovery, true, &selected, &error));
        QCOMPARE(selected.value("repos"), discovery.value("repos"));
        discovery.insert("complete", false); QVERIFY(!LauncherPrefix::selectInputs(discovery, false, &selected, &error));
    }
    void externalBuilder_data() {
        QTest::addColumn<bool>("includePrs");
        QTest::addColumn<bool>("nested");
        QTest::newRow("default-branches-legacy") << false << false; QTest::newRow("include-prs-legacy") << true << false;
        QTest::newRow("default-branches-nested") << false << true; QTest::newRow("include-prs-nested") << true << true;
    }
    void externalBuilder() {
        QFETCH(bool, includePrs); QFETCH(bool, nested);
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString source = temporary.path() + "/source", workspace = temporary.path() + "/new workspace";
        QVERIFY(QDir().mkpath(source));
        QString script = QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py";
        if (!nested) {
            QFile input(script); QVERIFY(input.open(QIODevice::ReadOnly)); auto bytes = input.readAll(); bytes.replace(", \"resolve-nested\", \"checkout-nested\"", "");
            script = temporary.path() + "/legacy.py"; QFile legacy(script); QVERIFY(legacy.open(QIODevice::WriteOnly)); legacy.write(bytes); legacy.close();
        }
        PrefixBuildRequest request{QStandardPaths::findExecutable("python3"), script, source, workspace, includePrs, 2, {"-DENABLE_TESTS=ON"}};
        PrefixBuilder builder; QSignalSpy completed(&builder, &PrefixBuilder::completed), phases(&builder, &PrefixBuilder::phaseChanged);
        builder.start(request); QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QVERIFY2(completed.first()[0].toBool(), qPrintable(completed.first()[1].toString())); QCOMPARE(phases.size(), nested ? 6 : 4);
        QCOMPARE(completed.first()[2].toString(), workspace + "/prefix");
        auto discovery = readJson(workspace + "/refs.discovery.lock.json"), selected = readJson(workspace + "/refs.lock.json");
        QCOMPARE(discovery.value("repos").toArray().first().toObject().value("prs").toArray().size(), 1);
        QCOMPARE(selected.value("repos").toArray().first().toObject().value("prs").toArray().size(), includePrs ? 1 : 0);
        auto provenance = readJson(workspace + "/prefix/.darling-launcher/build-provenance.json");
        QCOMPARE(provenance.value("script_sha256").toString().size(), 64);
        QCOMPARE(provenance.value("include_open_prs").toBool(), includePrs);
        if (nested) {
            auto lock = readJson(workspace + "/nested.refs.lock.json"), original = readJson(workspace + "/nested.discovery.lock.json");
            QCOMPARE(lock.value("vibedarling").toArray().first().toObject().value("prs").toArray().size(), includePrs ? 1 : 0);
            QCOMPARE(original.value("vibedarling").toArray().first().toObject().value("prs").toArray().size(), 1);
        }
        QCOMPARE(readJson(workspace + "/build-call.json").value("cmake_args").toArray(), QJsonArray{"-DENABLE_TESTS=ON"});
        QVERIFY(QDir(source).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }
    void stopOnIntegrationFailure() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString source = temporary.path() + "/source"; QVERIFY(QDir().mkpath(source));
        QFile marker(source + "/fail-checkout"); QVERIFY(marker.open(QIODevice::WriteOnly)); marker.close();
        PrefixBuilder builder; QSignalSpy completed(&builder, &PrefixBuilder::completed), phases(&builder, &PrefixBuilder::phaseChanged);
        builder.start({QStandardPaths::findExecutable("python3"), QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py", source, temporary.path() + "/new", true, 1, {}});
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000); QVERIFY(!completed.first()[0].toBool()); QCOMPARE(phases.size(), 3);
        QVERIFY(!QFileInfo::exists(temporary.path() + "/new/build-call.json"));
    }
    void refuseExistingOrSharedWorkspace() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString source = temporary.path() + "/source"; QVERIFY(QDir().mkpath(source));
        PrefixBuildRequest request{QStandardPaths::findExecutable("python3"), QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py", source, source, false, 1, {}}; QString error;
        QVERIFY(!LauncherPrefix::validateRequest(request, &error));
        request.workspace = source + "/new"; QVERIFY(!LauncherPrefix::validateRequest(request, &error));
        request.workspace = temporary.path() + "/new"; request.cmakeArguments = {"shell-command"}; QVERIFY(!LauncherPrefix::validateRequest(request, &error));
    }
};
QTEST_GUILESS_MAIN(PrefixTest)
#include "prefix_test.moc"

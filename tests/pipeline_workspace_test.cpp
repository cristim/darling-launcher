// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipelineworkspace.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>
class PipelineWorkspaceTest : public QObject {
    Q_OBJECT
    QTemporaryDir *temp = nullptr;
    QString base;
    static unsigned mode(const QString &p) { struct stat st {}; ::lstat(p.toLocal8Bit().constData(), &st); return st.st_mode & 07777; }
    static void put(const QString &path, const QByteArray &data) { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data); }
    PipelineWorkspace make() { QString error; auto ws = PipelineWorkspace::create(base, &error); [&] { QVERIFY2(ws.valid(), qPrintable(error)); }(); return ws; }
    HandoffStore::Release release(const PipelineWorkspace &ws, const QByteArray &body) {
        put(ws.stageDir("research") + "/OUTPUT.md", body);
        const auto r = HandoffStore::submit(ws.root, "research", "spec", ws.stageDir("research") + "/OUTPUT.md");
        [&] { QCOMPARE(r.status, HandoffStore::Status::Released); }();
        return {r.released, r.sha256, "research", "spec"};
    }
    static bool root() { return ::geteuid() == 0; }
private slots:
    void init() { temp = new QTemporaryDir; QVERIFY(temp->isValid()); base = temp->path() + "/missing-symbol"; }
    void cleanup() { QProcess::execute("chmod", {"-R", "u+rwx", temp->path()}); delete temp; }
    void createsPrivateLayout() {
        const auto ws = make();
        QCOMPARE(mode(ws.root), 0700u);
        for (const QString &d : HandoffStore::Stages + QStringList{"handoff"}) QCOMPARE(mode(ws.root + '/' + d), 0700u);
        QVERIFY(make().root != ws.root);
    }
    void lockShowsOnlyTheActiveStage() {
        auto ws = make(); QDir().mkdir(ws.root + "/spec.tainted");
        QCOMPARE(ws.lockFor("impl"), QString());
        QCOMPARE(mode(ws.root), 0100u); QCOMPARE(mode(ws.stageDir("impl")), 0700u);
        for (const QString &d : {"research", "spec", "validate", "handoff", "spec.tainted"}) QCOMPARE(mode(ws.root + '/' + d), 0u);
        if (!root()) { QVERIFY(QDir(ws.stageDir("research")).entryList().isEmpty()); QVERIFY(QDir(ws.root).entryList().isEmpty()); }
        bool tampered = true; QCOMPARE(ws.unlock(&tampered), QString()); QVERIFY(!tampered);
        for (const QString &d : {"research", "spec", "validate", "handoff", "spec.tainted", "."}) QCOMPARE(mode(ws.root + '/' + d), 0700u);
    }
    void chmodAndRestoreIsDetected() {
        auto ws = make(); QCOMPARE(ws.lockFor("impl"), QString());
        QTest::qWait(20); // ctime granularity
        ::chmod(ws.stageDir("research").toLocal8Bit().constData(), 0700); ::chmod(ws.stageDir("research").toLocal8Bit().constData(), 0);
        bool tampered = false; QCOMPARE(ws.unlock(&tampered), QString()); QVERIFY(tampered);
        QCOMPARE(ws.lockFor("impl"), QString());
        ::chmod(ws.root.toLocal8Bit().constData(), 0700); QDir().mkdir(ws.root + "/smuggled"); ::chmod(ws.root.toLocal8Bit().constData(), 0100);
        QCOMPARE(ws.unlock(&tampered), QString()); QVERIFY(tampered);
    }
    void importTraceVetsAndCopies() {
        const auto ws = make();
        put(temp->path() + "/bad.jsonl", "{\"symbol\": \"_f\", \"lr\": 4096}\n");
        QVERIFY(ws.importTrace(temp->path() + "/bad.jsonl").contains("non-compliant"));
        QVERIFY(QFile::link(temp->path() + "/bad.jsonl", temp->path() + "/link.jsonl"));
        QVERIFY(!ws.importTrace(temp->path() + "/link.jsonl").isEmpty());
        put(temp->path() + "/good.jsonl", "{\"symbol\": \"_CFFoo\", \"regs\": [\"0x2\"]}\n");
        QCOMPARE(ws.importTrace(temp->path() + "/good.jsonl"), QString());
        QCOMPARE(mode(ws.stageDir("research") + "/trace/T0.jsonl"), 0600u); QCOMPARE(mode(ws.stageDir("research") + "/trace"), 0700u);
        QVERIFY(!ws.importTrace(temp->path() + "/good.jsonl").isEmpty()); // never overwrites
        for (const QByteArray &line : {QByteArray("{\"symbol\": \"_f\", \"stack\": [1]}"), QByteArray("{\"symbol\": \"_f\", \"regs\": {\"x30\": 1}}"), QByteArray("{\"symbol\": \"_f\", \"\uff4c\uff52\": 1}"), QByteArray("not json")}) {
            const auto other = make(); put(temp->path() + "/t.jsonl", "{\"symbol\": \"_ok\", \"count\": 2}\n" + line + "\n");
            QVERIFY2(other.importTrace(temp->path() + "/t.jsonl").contains("line 2"), line.constData());
        }
    }
    void deliverChecksHashAndNeverFollowsLinks() {
        const auto ws = make(); const auto r = release(ws, "facts\n");
        QCOMPARE(ws.deliver(r, "spec"), QString());
        QCOMPARE(mode(ws.stageDir("spec") + "/inputs/" + r.name), 0600u);
        QVERIFY(!ws.deliver(r, "spec").isEmpty());
        QDir().mkpath(ws.stageDir("impl") + "/inputs"); put(temp->path() + "/target", "keep");
        QVERIFY(QFile::link(temp->path() + "/target", ws.stageDir("impl") + "/inputs/" + r.name));
        QVERIFY(!ws.deliver(r, "impl").isEmpty());
        QFile t(temp->path() + "/target"); t.open(QIODevice::ReadOnly); QCOMPARE(t.readAll(), QByteArray("keep"));
        ::chmod((ws.root + "/handoff/" + r.name).toLocal8Bit().constData(), 0600); put(ws.root + "/handoff/" + r.name, "edited\n");
        QVERIFY(ws.deliver(r, "validate").contains("no longer matches"));
    }
    void firewallFindsResearchMaterial() {
        const auto ws = make();
        put(ws.stageDir("research") + "/notes.md", "private research notes\n");
        const auto r = release(ws, "T1 facts body\n");
        QVERIFY(ws.firewall("impl", {}).isEmpty());
        put(ws.stageDir("impl") + "/copy.md", "private research notes\n");
        QDir().mkpath(ws.stageDir("impl") + "/x"); QFile::copy(ws.root + "/handoff/" + r.name, ws.stageDir("impl") + "/x/t1.md");
        put(ws.stageDir("impl") + "/trace.txt", "{\"symbol\": \"_f\", \"x0\": 1}\n");
        QDir().mkpath(ws.stageDir("impl") + "/inputs"); put(ws.stageDir("impl") + "/inputs/extra.md", "x\n");
        put(ws.stageDir("impl") + "/padded.txt", "{\"symbol\": \"_f\", \"x0\": 1}\n" + QByteArray(2 * 1024 * 1024, ' '));
        put(ws.stageDir("impl") + "/huge.bin", QByteArray(9 * 1024 * 1024, 'a'));
        const QString problems = ws.firewall("impl", {}).join('\n');
        for (const QString &p : {"copy.md is a copy", "x/t1.md is a copy", "trace.txt contains raw trace", "inputs/extra.md was not delivered", "padded.txt contains raw trace", "huge.bin is too large"}) QVERIFY2(problems.contains(p), qPrintable(problems));
    }
    void taintMovesNeverDeletes() {
        const auto ws = make(); put(ws.stageDir("spec") + "/OUTPUT.md", "x");
        QVERIFY(!ws.taint("spec", "../escape").isEmpty());
        QCOMPARE(ws.taint("spec", "spec-claude-1-abcd"), QString());
        QVERIFY(QFileInfo::exists(ws.root + "/spec.tainted/spec-claude-1-abcd/OUTPUT.md")); QVERIFY(QDir(ws.stageDir("spec")).isEmpty());
        HandoffStore::appendEvent(ws.root, "TAINT", {{"report_to_compliance", true}});
        QVERIFY(HandoffStore::verifyChain(ws.root).summary.endsWith("tainted_stages=1 compliance_reports=1 recon_releases=0"));
    }
    void findsProcessesInTheRunFolder() {
        const auto ws = make(); QProcess p; p.setWorkingDirectory(ws.stageDir("impl")); p.start("sleep", {"30"}); QVERIFY(p.waitForStarted());
        QVERIFY(PipelineWorkspace::processesUsing(ws.root).contains(p.processId()));
        p.kill(); p.waitForFinished();
        QVERIFY(!PipelineWorkspace::processesUsing(ws.root).contains(p.processId()));
    }
    void recoversRunsLeftLocked() {
        PipelineWorkspace ws = make(); QCOMPARE(ws.lockFor("spec"), QString());
        PipelineWorkspace live = make(); QCOMPARE(live.lockFor("spec"), QString());
        const int held = live.holdRunLock(); QVERIFY(held >= 0); // a live run, maybe in another launcher process
        PipelineWorkspace other = make();
        const QStringList restored = PipelineWorkspace::recover(base);
        QCOMPARE(mode(live.root), 0100u); ::close(held);
        QCOMPARE(restored, QStringList{ws.root});
        QCOMPARE(mode(ws.root), 0700u); QCOMPARE(mode(ws.stageDir("research")), 0700u);
        QVERIFY(HandoffStore::verifyChain(ws.root).summary.startsWith("records=1 "));
    }
};
QTEST_GUILESS_MAIN(PipelineWorkspaceTest)
#include "pipeline_workspace_test.moc"

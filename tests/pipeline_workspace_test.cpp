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
    void baseIsUnlistableAndFinishedRunsClose() {
        auto ws = make();
        QCOMPARE(mode(base), 0300u);
        QString closed; QCOMPARE(ws.close(&closed), QString()); QCOMPARE(mode(closed), 0u); QVERIFY(!QFileInfo::exists(ws.root)); QVERIFY(closed.startsWith(ws.root + ".closed-"));
        QCOMPARE(PipelineWorkspace::recover(base), QStringList()); QCOMPARE(mode(closed), 0u); QCOMPARE(mode(base), 0300u); // closed runs stay closed
        auto live = make(); QCOMPARE(live.lockFor("impl"), QString());
        ::chmod(base.toLocal8Bit().constData(), 0700); // an agent makes the base listable
        bool tampered = false; QCOMPARE(live.unlock(&tampered), QString()); QVERIFY(tampered); QCOMPARE(mode(base), 0300u);
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
        put(temp->path() + "/good.jsonl", "{\"symbol\": \"_CFFoo\", \"library\": \"CoreFoundation\", \"arch\": \"arm64\", \"count\": 3, \"seq\": 1, \"regs\": [\"0x2\", 7], \"fp_regs\": [\"0x3ff0000000000000\"], \"x8\": \"0x10\", \"weak\": false}\n{\"sym\": \"_g\", \"regs\": {\"rdi\": 1, \"rsi\": \"0xff\"}}\n");
        QCOMPARE(ws.importTrace(temp->path() + "/good.jsonl"), QString());
        QCOMPARE(mode(ws.stageDir("research") + "/trace/T0.jsonl"), 0600u); QCOMPARE(mode(ws.stageDir("research") + "/trace"), 0700u);
        QVERIFY(!ws.importTrace(temp->path() + "/good.jsonl").isEmpty()); // never overwrites
        // compliance ruling F1: a stack-word-sized array and a dereferenced string under allowed keys are refused
        for (const QByteArray &line : {QByteArray("{\"symbol\": \"_f\", \"args\": [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]}"), QByteArray("{\"symbol\": \"_f\", \"x1\": \"com.apple.secret.key\"}"),
                                       QByteArray("{\"symbol\": \"_f\", \"regs\": [\"Hello\"]}"), QByteArray("{\"symbol\": \"_f\", \"image\": \"CoreFoundation\"}"), QByteArray("{\"symbol\": \"_f\", \"position\": 4096}"),
                                       QByteArray("{\"symbol\": \"_f\", \"regs\": {\"x0\": {\"x1\": 1}}}"), QByteArray("{\"symbol\": \"" + QByteArray(600, 'a') + "\"}"), QByteArray("{\"symbol\": \"_f\", \"q0\": \"0x" + QByteArray(40, '1') + "\"}"),
                                       QByteArray("{\"symbol\": \"_f\", \"words\": [1]}"), QByteArray("{\"symbol\": \"_f\", \"pad\": \"") + QByteArray(70000, 'a') + "\"}",
                                       QByteArray("{\"symbol\": \"_f\", \"stack\": [1]}"), QByteArray("{\"symbol\": \"_f\", \"regs\": {\"x30\": 1}}"), QByteArray("{\"symbol\": \"_f\", \"\uff4c\uff52\": 1}"), QByteArray("not json")}) {
            const auto other = make(); put(temp->path() + "/t.jsonl", "{\"symbol\": \"_ok\", \"count\": 2}\n" + line + "\n");
            QVERIFY2(other.importTrace(temp->path() + "/t.jsonl").contains("line 2"), line.constData());
        }
    }
    void importTraceRejectsDuplicateKeys() {
        for (const QByteArray &line : {QByteArray("{\"symbol\": \"s\", \"count\": \"0x7ffd12345678 backtrace stuff\", \"count\": 1}"),
                                       QByteArray("{\"symbol\": \"s\", \"regs\": {\"x0\": \"zzz\", \"x0\": 1}}"),
                                       QByteArray("{\"symbol\": \"s\", \"cou\\u006et\": \"payload\", \"count\": 1}"),
                                       QByteArray("{\"symbol\": \"s\", \"regs\": [{\"x0\": 1, \"x0\": 2}]}")}) {
            const auto ws = make(); put(temp->path() + "/dup.jsonl", "{\"symbol\": \"_ok\", \"count\": 2}\n" + line + "\n");
            const QString error = ws.importTrace(temp->path() + "/dup.jsonl");
            QVERIFY2(error.contains("line 2") && error.contains("repeats the key"), line.constData());
            QVERIFY(!QFileInfo::exists(ws.stageDir("research") + "/trace/T0.jsonl"));
        }
    }
    void importTraceKeepsNormalTraces() {
        const auto ws = make();
        put(temp->path() + "/ok.jsonl", "{\"symbol\": \"_a\", \"regs\": {\"rdi\": 1}, \"fp_regs\": [\"0x3ff0000000000000\"]}\n{\"symbol\": \"_b\", \"count\": 1}\n");
        QCOMPARE(ws.importTrace(temp->path() + "/ok.jsonl"), QString());
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
    void recoveryLeavesARunLockedWhileSomethingUsesIt() {
        PipelineWorkspace ws = make(); QCOMPARE(ws.lockFor("impl"), QString());
        QProcess agent; agent.setWorkingDirectory(ws.stageDir("impl")); agent.start("sleep", {"30"}); QVERIFY(agent.waitForStarted());
        QCOMPARE(PipelineWorkspace::recover(base), QStringList());
        QCOMPARE(mode(ws.root), 0100u); QCOMPARE(mode(ws.stageDir("research")), 0u);
        agent.kill(); agent.waitForFinished();
        QCOMPARE(PipelineWorkspace::recover(base), QStringList{ws.root});
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

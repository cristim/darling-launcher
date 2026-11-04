// SPDX-License-Identifier: GPL-3.0-or-later
// The pipeline state machine against fake claude/codex/opencode CLIs (shell scripts on PATH). The
// fake reads its stage from its working directory and its behaviour from FAKE/plan:
//   "<stage> <launch|*> <mode>"
#include "pipeline.h"
#include "stagesupervisor.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QtTest>
#include <cstring>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
using State = MissingSymbolPipeline::State;
namespace {
const char *FakeAgent = R"SH(#!/bin/sh
F=@FAKE@
stage=$(basename "$PWD")
n=$(cat "$F/count-$stage" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "$F/count-$stage"
echo "$(basename "$0") $stage $n" >> "$F/calls"
mode=$(awk -v s="$stage" -v n="$n" '$1==s && ($2==n || $2=="*") {print $3; exit}' "$F/plan")
[ -n "$mode" ] || mode=ok
out() {
  case $stage in
    research) printf 'Facts: _CFFoo called 3 times, first at position 1 (attempt %s).\nSources: nm -u of the fixture app\n' "$n" ;;
    spec) printf 'Requirement 1 (rung 3): _CFFoo returns NULL for an empty key (attempt %s).\nSources: public CFFoo documentation\n' "$n" ;;
    impl) printf 'Implemented _CFFoo from the specification with a fixture test (attempt %s).\nSources: specification\n' "$n" ;;
    validate) printf 'VERDICT: PASS\nThe guest behaviour matches the specification (attempt %s).\n' "$n" ;;
  esac > OUTPUT.md
}
case $mode in
  ok) out ;;
  taint) printf 'Notes: the function calls sub_1A2B first.\n' > OUTPUT.md ;;
  revise) printf 'Loaded from /System/Library/Frameworks/Foo.framework (attempt %s)\n' "$n" > OUTPUT.md ;;
  selftaint) out; echo 'I opened a disassembly listing by accident.' > TAINTED.md ;;
  logtaint) out; echo 'Contents of (__TEXT,__text) section' ;;
  mismatch) printf 'VERDICT: MISMATCH\nThe guest sees an error for an empty key (attempt %s).\n' "$n" > OUTPUT.md ;;
  silent) exit 3 ;;
  symlink) ln -s ../research/trace/T0.jsonl OUTPUT.md ;;
  fifo) mkfifo OUTPUT.md ;;
  hardlink) ln "$F/outside.md" OUTPUT.md ;;
  chmod) chmod 700 ../research 2>/dev/null; ls ../research > /dev/null 2>&1; chmod 000 ../research 2>/dev/null; out ;;
  sleep) sleep 600 ;;
  escape) F="$F" setsid sh -c 'echo $$ > "$F/bgpid"; cd /; exec sleep 600' > /dev/null 2>&1 < /dev/null & sleep 1; out ;;
  checkbg) if kill -0 "$(cat "$F/bgpid")" 2>/dev/null; then echo alive > "$F/bg-state"; else echo dead > "$F/bg-state"; fi; out ;;
  probe)
    for p in ../research ../research/trace/T0.jsonl ../handoff ../handoff/01-research-to-spec.md ../spec ../spec/OUTPUT.md ..; do
      if ls "$p" > /dev/null 2>&1 || cat "$p" > /dev/null 2>&1; then echo "READ $p"; else echo "DENIED $p"; fi
    done > "$F/probe-$(basename "$0")"
    cp -r ../research "$F/stolen-$(basename "$0")" 2> /dev/null && echo COPIED >> "$F/probe-$(basename "$0")"
    find . -type f | sort > "$F/listing-$(basename "$0")"
    out ;;
esac
)SH";
}
class PipelineTest : public QObject {
    Q_OBJECT
    QTemporaryDir *temp = nullptr;
    QString fake, bin, base, oldPath;
    void plan(const QString &text) { QFile f(fake + "/plan"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(text.toUtf8()); }
    QString read(const QString &path) const { QFile f(path); return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString(); }
    QList<QJsonObject> audit(const PipelineWorkspace &ws) const {
        QList<QJsonObject> out;
        for (const QString &l : read(ws.root + "/handoff/AUDIT.jsonl").split('\n')) if (!l.isEmpty()) out << QJsonDocument::fromJson(l.toUtf8()).object();
        return out;
    }
    QStringList verdicts(const PipelineWorkspace &ws) const { QStringList v; for (const auto &r : audit(ws)) v << r.value("verdict").toString(); return v; }
    struct Run { State state; QString message; PipelineWorkspace ws; };
    Run run(const QStringList &agents = {"claude", "codex", "opencode", "claude"}, int timeoutSeconds = 60, std::function<void(MissingSymbolPipeline &)> during = {}) {
        QString error; const auto ws = PipelineWorkspace::create(base, &error);
        MissingSymbolPipeline::Options o; o.agents = agents; o.trace = fake + "/trace.jsonl"; o.symbol = "_CFFoo"; o.library = "CoreFoundation"; o.stageTimeoutSeconds = timeoutSeconds;
        MissingSymbolPipeline p(ws, o);
        QSignalSpy done(&p, &MissingSymbolPipeline::finished);
        p.start();
        if (during) during(p);
        if (done.isEmpty()) done.wait(120000);
        return {p.state(), done.isEmpty() ? QString("no finish") : done[0][1].toString(), p.workspace()};
    }
private slots:
    void initTestCase() { qRegisterMetaType<MissingSymbolPipeline::State>(); }
    void init() {
        temp = new QTemporaryDir; QVERIFY(temp->isValid());
        fake = temp->path() + "/fake"; bin = temp->path() + "/bin"; base = temp->path() + "/missing-symbol";
        QDir().mkpath(fake); QDir().mkpath(bin);
        for (const QString &name : {"claude", "codex", "opencode"}) {
            QFile f(bin + '/' + name); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(QByteArray(FakeAgent).replace("@FAKE@", fake.toUtf8())); f.close();
            QVERIFY(f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        }
        QFile trace(fake + "/trace.jsonl"); QVERIFY(trace.open(QIODevice::WriteOnly)); trace.write("{\"symbol\": \"_CFFoo\", \"image\": \"CoreFoundation\", \"regs\": [\"0x0\"]}\n");
        QFile outside(fake + "/outside.md"); QVERIFY(outside.open(QIODevice::WriteOnly)); outside.write("outside\n");
        plan("");
        oldPath = qEnvironmentVariable("PATH"); qputenv("PATH", (bin + ":/usr/bin:/bin").toUtf8());
    }
    void cleanup() { qputenv("PATH", oldPath.toUtf8()); QProcess::execute("chmod", {"-R", "u+rwx", temp->path()}); delete temp; }
    void happyPathReleasesFourHandoffsWithAVerifiedChain() {
        const auto r = run();
        QCOMPARE(r.state, State::Done);
        QStringList releases; for (const auto &rec : audit(r.ws)) if (rec.value("verdict") == "RELEASE") releases << rec.value("output").toString();
        QCOMPARE(releases, QStringList({"01-research-to-spec.md", "02-spec-to-impl.md", "03-impl-to-validate.md"}));
        QCOMPARE(verdicts(r.ws).first(), QString("START")); QCOMPARE(verdicts(r.ws).last(), QString("DONE"));
        QCOMPARE(HandoffStore::verifyChain(r.ws.root).exitCode, 0);
        QCOMPARE(read(fake + "/calls"), QString("claude research 1\ncodex spec 1\nopencode impl 1\nclaude validate 1\n"));
        QVERIFY(QFileInfo::exists(r.ws.stageDir("validate") + "/inputs/02-spec-to-impl.md")); // spec delivered, not cited
        for (const auto &rec : audit(r.ws)) if (rec.value("verdict") == "RELEASE" && rec.value("from") == "validate") QFAIL("validate released nothing");
        const QString python = QStandardPaths::findExecutable("python3");
        if (python.isEmpty()) QSKIP("python3 not found: the python oracle did NOT verify the pipeline's chain");
        QCOMPARE(QProcess::execute(python, {TEST_SOURCE_DIR "/tests/handoff-oracle/check_handoff.py", "--verify-chain", "--run-root", r.ws.root}), 0);
    }
    void implementationNeverSeesResearchMaterial_data() {
        QTest::addColumn<QString>("cli");
        for (const char *c : {"claude", "codex", "opencode"}) QTest::newRow(c) << c;
    }
    void implementationNeverSeesResearchMaterial() {
        QFETCH(QString, cli);
        plan("impl 1 probe\n");
        const auto r = run({"claude", "codex", cli, "claude"});
        QCOMPARE(r.state, State::Done);
        const QString listing = read(fake + "/listing-" + cli);
        QVERIFY2(listing.contains("./inputs/02-spec-to-impl.md") && !listing.contains("research-to-spec") && !listing.contains("T0"), qPrintable(listing));
        if (::geteuid() == 0) QSKIP("running as root: directory modes do not stop root, the access probe is not meaningful");
        const QString probe = read(fake + "/probe-" + cli);
        QVERIFY2(!probe.contains("READ") && !probe.contains("COPIED") && probe.count("DENIED") == 7, qPrintable(probe));
        QVERIFY(!QFileInfo::exists(fake + "/stolen-" + cli + "/trace"));
    }
    void taintedStageIsMovedAsideAndReplaced() {
        plan("spec 1 taint\n");
        const auto r = run();
        QCOMPARE(r.state, State::Done);
        const QStringList tainted = QDir(r.ws.root + "/spec.tainted").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(tainted.size(), 1); QVERIFY(tainted[0].startsWith("spec-codex-1-"));
        QVERIFY(read(r.ws.root + "/spec.tainted/" + tainted[0] + "/OUTPUT.md").contains("sub_1A2B"));
        QVERIFY(read(fake + "/calls").contains("opencode spec 2")); // replacement uses another provider
        const auto recs = audit(r.ws);
        QVERIFY(std::any_of(recs.begin(), recs.end(), [](const QJsonObject &o) { return o.value("verdict") == "TAINT" && o.value("report_to_compliance").toBool(); }));
        QVERIFY(HandoffStore::verifyChain(r.ws.root).summary.contains("tainted_stages=1"));
        QVERIFY(!read(r.ws.root + "/handoff/02-spec-to-impl.md").contains("sub_1A2B"));
    }
    void selfReportedAndLoggedExposureTaint_data() { QTest::addColumn<QString>("mode"); QTest::newRow("TAINTED.md") << "selftaint"; QTest::newRow("AGENT.log") << "logtaint"; }
    void selfReportedAndLoggedExposureTaint() {
        QFETCH(QString, mode); plan("research 1 " + mode + "\n");
        const auto r = run();
        QCOMPARE(r.state, State::Done); QCOMPARE(QDir(r.ws.root + "/research.tainted").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
        QVERIFY(QFileInfo::exists(r.ws.stageDir("research") + "/trace/T0.jsonl")); // fresh workspace gets the trace again
    }
    void rejectedOutputIsRevisedThenFails() {
        plan("research 1 revise\n");
        auto r = run(); QCOMPARE(r.state, State::Done);
        QVERIFY(verdicts(r.ws).contains("REJECT")); QVERIFY(QDir(r.ws.root + "/research.tainted").entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(read(r.ws.stageDir("research") + "/REJECTED.txt").contains("[R6]"));
        QFile::remove(fake + "/count-research"); plan("research * revise\n");
        r = run(); QCOMPARE(r.state, State::Failed); QVERIFY(r.message.contains("rejected 3 times"));
        QCOMPARE(verdicts(r.ws).count("REJECT"), MissingSymbolPipeline::MaxRevisions + 1);
    }
    void validationMismatchGoesBackToImplementation() {
        plan("validate 1 mismatch\n");
        const auto r = run(); QCOMPARE(r.state, State::Done);
        QVERIFY(QFileInfo::exists(r.ws.root + "/handoff/04-validate-to-impl.md"));
        QVERIFY(QFileInfo::exists(r.ws.stageDir("impl") + "/inputs/04-validate-to-impl.md"));
        QVERIFY(read(fake + "/calls").contains("impl 2"));
    }
    void outputTricksTaint_data() { QTest::addColumn<QString>("mode"); for (const char *m : {"symlink", "fifo", "hardlink", "chmod"}) QTest::newRow(m) << m; }
    void outputTricksTaint() {
        QFETCH(QString, mode);
        if (mode == "chmod" && ::geteuid() == 0) QSKIP("root ignores modes");
        plan("spec 1 " + mode + "\n");
        const auto r = run(); QCOMPARE(r.state, State::Done);
        QCOMPARE(QDir(r.ws.root + "/spec.tainted").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
        QVERIFY(!read(r.ws.root + "/handoff/02-spec-to-impl.md").contains("CFFoo\", \"image\""));
    }
    void tooManyTaintsFail() {
        plan("research * taint\n");
        const auto r = run(); QCOMPARE(r.state, State::Failed);
        QCOMPARE(QDir(r.ws.root + "/research.tainted").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), MissingSymbolPipeline::MaxReplacements + 1);
        QCOMPARE(verdicts(r.ws).last(), QString("FAILED"));
    }
    void escapedBackgroundProcessIsKilledBeforeTheNextStage() {
        plan("research 1 escape\nspec 1 checkbg\n");
        const auto r = run(); QCOMPARE(r.state, State::Done);
        QCOMPARE(read(fake + "/bg-state").trimmed(), QString("dead"));
        QVERIFY(read(r.ws.stageDir("research") + "/AGENT.log").contains("left running"));
    }
    void cancelStopsTheAgent() {
        plan("research 1 sleep\n");
        const auto r = run({"claude", "codex", "opencode", "claude"}, 60, [](MissingSymbolPipeline &p) { QTimer::singleShot(1500, &p, &MissingSymbolPipeline::cancel); });
        QCOMPARE(r.state, State::Cancelled); QCOMPARE(verdicts(r.ws).last(), QString("CANCEL"));
        QCOMPARE(PipelineWorkspace::processesUsing(r.ws.root), QList<qint64>());
        QVERIFY(QFileInfo(r.ws.root).permissions().testFlag(QFile::ReadOwner)); // unlocked
    }
    void timeoutStopsTheAgent() {
        plan("spec 1 sleep\n");
        const auto r = run({"claude", "codex", "opencode", "claude"}, 2);
        QCOMPARE(r.state, State::TimedOut); QCOMPARE(verdicts(r.ws).last(), QString("TIMEOUT"));
        QCOMPARE(PipelineWorkspace::processesUsing(r.ws.root), QList<qint64>());
    }
    void silentAgentIsRevisedThenFails() {
        plan("impl * silent\n");
        const auto r = run(); QCOMPARE(r.state, State::Failed); QVERIFY(r.message.contains("no OUTPUT.md"));
    }
    void editedAuditLogStopsTheRun() {
        plan("spec 1 sleep\n");
        const auto r = run({"claude", "codex", "opencode", "claude"}, 3, [](MissingSymbolPipeline &p) {
            QTimer::singleShot(1000, &p, [&p] {
                const QString root = p.workspace().root; ::chmod(root.toLocal8Bit().constData(), 0700); ::chmod((root + "/handoff").toLocal8Bit().constData(), 0700);
                QFile a(root + "/handoff/AUDIT.jsonl"); a.open(QIODevice::Append); a.write("{\"verdict\": \"FORGED\"}\n"); a.close();
                ::chmod((root + "/handoff").toLocal8Bit().constData(), 0); ::chmod(root.toLocal8Bit().constData(), 0100);
            });
        });
        QCOMPARE(r.state, State::Failed); QVERIFY2(r.message.contains("audit log changed") || r.message.contains("changed"), qPrintable(r.message));
    }
    void forbiddenTraceIsRefused() {
        QFile trace(fake + "/trace.jsonl"); QVERIFY(trace.open(QIODevice::WriteOnly)); trace.write("{\"symbol\": \"_f\", \"pc\": \"0x1000\"}\n"); trace.close();
        const auto r = run(); QCOMPARE(r.state, State::Failed); QVERIFY(r.message.contains("non-compliant"));
        QVERIFY(!QFileInfo::exists(fake + "/calls"));
    }
};
int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], StageSupervisor::Flag) == 0) return StageSupervisor::run(argc, argv);
    QApplication app(argc, argv);
    PipelineTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "pipeline_test.moc"

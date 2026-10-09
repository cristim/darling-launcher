// SPDX-License-Identifier: GPL-3.0-or-later
// Mirrors the oracle's CliTests/ChainTests/ProvenanceTests against the C++ store, then checks that
// python and C++ verify each other's audit chains.
#include "handoffstore.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>
using HandoffStore::Status;
class HandoffStoreTest : public QObject {
    Q_OBJECT
    QTemporaryDir *temp = nullptr;
    QString root, hand;
    int counter = 0;
    static void put(const QString &path, const QByteArray &data) { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data); }
    static unsigned mode(const QString &p) { struct stat st {}; ::lstat(p.toLocal8Bit().constData(), &st); return st.st_mode & 07777; }
    QString write(const QByteArray &text, const QString &stage = "research", const QString &name = "in.md") {
        QDir().mkpath(root + '/' + stage); ::chmod((root + '/' + stage).toLocal8Bit().constData(), 0700);
        const QString p = root + '/' + stage + '/' + name; QFile::remove(p); put(p, text); return p;
    }
    HandoffStore::Result submit(const QString &from, const QString &to, QByteArray text, const QString &agent = {}, const QStringList &inputs = {}, bool unique = true) {
        if (unique) text += "\nnote " + QByteArray::number(counter++) + "\n";
        return HandoffStore::submit(root, from, to, write(text, from), agent.isEmpty() && (from == "impl" || from == "validate") ? "test-agent" : agent, inputs);
    }
    QList<QJsonObject> audit() const {
        QList<QJsonObject> out; QFile f(hand + "/AUDIT.jsonl"); f.open(QIODevice::ReadOnly);
        for (const QByteArray &l : f.readAll().split('\n')) if (!l.isEmpty()) out << QJsonDocument::fromJson(l).object();
        return out;
    }
    QStringList listing() const { auto l = QDir(hand).entryList(QDir::Files | QDir::Hidden); l.removeAll("AUDIT.jsonl"); return l; }
    QStringList auditLines() const { QFile f(hand + "/AUDIT.jsonl"); f.open(QIODevice::ReadOnly); QStringList l = QString::fromUtf8(f.readAll()).split('\n'); l.removeAll(""); return l; }
    void setAuditLines(const QStringList &lines) { put(hand + "/AUDIT.jsonl", (lines.join('\n') + '\n').toUtf8()); }
    QString rules(const HandoffStore::Result &r) { QStringList l; for (const auto &h : r.reasons) l << QString("%1:%2").arg(h.line).arg(h.rule); return l.join(' '); }
    const QByteArray clean = "Behaviour spec.\nSources: public docs; Libc\n";
    void threeReleases() {
        for (const QString &s : QStringList{"research>spec", "spec>impl", "impl>validate"}) QCOMPARE(submit(s.section('>', 0, 0), s.section('>', 1), "clean " + s.toUtf8() + "\nSources: docs\n").status, Status::Released);
        QCOMPARE(submit("spec", "impl", "sub_ABCD\n").status, Status::Rejected);
        QCOMPARE(HandoffStore::verifyChain(root).exitCode, 0);
    }
private slots:
    void init() {
        temp = new QTemporaryDir; QVERIFY(temp->isValid());
        root = temp->path() + "/run"; hand = root + "/handoff"; QVERIFY(QDir().mkdir(root)); ::chmod(root.toLocal8Bit().constData(), 0700);
    }
    void cleanup() { QProcess::execute("chmod", {"-R", "u+rwx", temp->path()}); delete temp; temp = nullptr; }
    void releaseAndSequence() {
        auto r = submit("research", "spec", clean); QCOMPARE(r.status, Status::Released); QCOMPARE(r.released, QString("01-research-to-spec.md"));
        QCOMPARE(submit("spec", "impl", clean).status, Status::Released);
        QCOMPARE(listing(), QStringList({"01-research-to-spec.md", "02-spec-to-impl.md", "MANIFEST.txt"}));
        const auto recs = audit();
        QCOMPARE(recs[0].value("verdict").toString(), QString("RELEASE")); QCOMPARE(recs[0].value("sources").toArray(), QJsonArray({"public docs", "Libc"}));
        QCOMPARE(recs[0].value("prev_hash").toString(), HandoffStore::Genesis); QVERIFY(recs[0].value("agent").isNull()); QCOMPARE(recs[0].value("inputs").toArray(), QJsonArray());
        QCOMPARE(HandoffStore::verifyChain(root).exitCode, 0);
        QCOMPARE(mode(hand), 0700u);
        for (const QString &n : {"AUDIT.jsonl", "MANIFEST.txt", "01-research-to-spec.md"}) QCOMPARE(mode(hand + '/' + n), 0600u);
    }
    void transitions() {
        for (const QString &t : QStringList{"research>impl", "research>validate", "spec>research", "impl>research", "impl>impl", "research>research", "validate>research", "validate>validate"}) {
            const auto r = submit(t.section('>', 0, 0), t.section('>', 1), clean); QCOMPARE(r.status, Status::Rejected); QVERIFY2(rules(r).contains("X1"), qPrintable(t));
        }
        for (const QString &t : QStringList{"research>spec", "spec>impl", "impl>validate", "spec>validate", "impl>spec", "validate>impl", "validate>spec"})
            QVERIFY2(submit(t.section('>', 0, 0), t.section('>', 1), clean).status == Status::Released, qPrintable(t));
    }
    void rejectLogsWithoutChangingHandoff() {
        submit("research", "spec", clean); const auto before = listing();
        const auto r = submit("spec", "impl", "see sub_1234 here\nmore\n0x100003f40: x\n");
        QCOMPARE(r.status, Status::Rejected); QVERIFY(rules(r).contains("1:R5")); QVERIFY(rules(r).contains("3:R2")); QVERIFY(r.reportToCompliance);
        for (const auto &h : r.reasons) QVERIFY(!h.description.contains("1234"));
        QCOMPARE(listing(), before);
        const auto last = audit().last();
        QCOMPARE(last.value("verdict").toString(), QString("REJECT")); QCOMPARE(last.value("rules").toArray(), QJsonArray({"R2", "R5"}));
        QVERIFY(last.value("output_sha256").isNull()); QVERIFY(last.value("report_to_compliance").toBool());
        QCOMPARE(HandoffStore::verifyChain(root).exitCode, 0);
    }
    void launcherOnlyRulesAreRecordedWithTheirInnerRule() {
        const auto r = submit("research", "spec", "Attachment: bW92IHgwLCB4MQpsZHIgeDIsIFtzcCwgIzhdCmJsIF9mb28K\n");
        QCOMPARE(r.status, Status::Rejected); QVERIFY(r.reportToCompliance);
        QCOMPARE(audit().last().value("rules").toArray(), QJsonArray({"E2/R1"}));
    }
    void symlinkedPathsRefused() {
        const QString elsewhere = temp->path() + "/elsewhere"; QDir().mkdir(elsewhere);
        QVERIFY(QFile::link(elsewhere, hand));
        auto r = submit("research", "spec", clean); QCOMPARE(r.status, Status::Unsafe); QVERIFY(r.error.contains("symlink"));
        QVERIFY(QDir(elsewhere).isEmpty()); QFile::remove(hand);
        QDir().mkdir(hand); ::chmod(hand.toLocal8Bit().constData(), 0700);
        const QString stolen = temp->path() + "/stolen.jsonl"; put(stolen, ""); QVERIFY(QFile::link(stolen, hand + "/AUDIT.jsonl"));
        QCOMPARE(submit("research", "spec", clean).status, Status::Unsafe); QCOMPARE(QFileInfo(stolen).size(), 0);
        QFile::remove(hand + "/AUDIT.jsonl");
        const QString link = temp->path() + "/link"; QVERIFY(QFile::link(root, link)); write(clean);
        for (const QString &r2 : {link, link + "/"}) { const auto res = HandoffStore::submit(r2, "research", "spec", root + "/research/in.md"); QCOMPARE(res.status, Status::Unsafe); QVERIFY(res.error.contains("symlink")); }
    }
    void inputLocationAndTypeTricksRefused() {
        const QString t1 = write(clean, "research", "T1.md"); write(clean, "spec", "ok.md");
        auto r = HandoffStore::submit(root, "spec", "impl", t1); QCOMPARE(r.status, Status::Unsafe); QVERIFY(r.error.contains("outside the spec workspace"));
        const QString outside = temp->path() + "/o.md"; put(outside, clean);
        QCOMPARE(HandoffStore::submit(root, "spec", "impl", outside).status, Status::Unsafe);
        QCOMPARE(HandoffStore::submit(root, "spec", "impl", root + "/spec/../research/T1.md").status, Status::Unsafe);
        QVERIFY(QFile::link(t1, root + "/spec/l.md")); QCOMPARE(HandoffStore::submit(root, "spec", "impl", root + "/spec/l.md").status, Status::Unsafe);
        QVERIFY(::link(outside.toLocal8Bit().constData(), (root + "/spec/hard.md").toLocal8Bit().constData()) == 0);
        r = HandoffStore::submit(root, "spec", "impl", root + "/spec/hard.md"); QCOMPARE(r.status, Status::Unsafe); QVERIFY(r.error.contains("hard links"));
        QVERIFY(::mkfifo((root + "/spec/fifo.md").toLocal8Bit().constData(), 0600) == 0);
        QElapsedTimer timer; timer.start();
        QCOMPARE(HandoffStore::submit(root, "spec", "impl", root + "/spec/fifo.md").status, Status::Unsafe); QVERIFY(timer.elapsed() < 5000);
        QDir().mkdir(root + "/research/sub");
        write(QByteArray(HandoffStore::MaxInput + 1, 'a'), "research", "big.md");
        for (const QString &f : {root + "/research/big.md", root + "/research/sub", root + "/research/missing.md"}) QCOMPARE(HandoffStore::submit(root, "research", "spec", f).status, Status::Unsafe);
        QVERIFY(!QFileInfo::exists(hand + "/01-spec-to-impl.md"));
    }
    void runRootRules() {
        const QString typo = temp->path() + "/typo"; put(temp->path() + "/f.md", clean);
        QCOMPARE(HandoffStore::submit(typo, "research", "spec", temp->path() + "/f.md").status, Status::Unsafe); QVERIFY(!QFileInfo::exists(typo));
        auto r = HandoffStore::submit(root, "research", "spec", temp->path() + "/f.md"); QCOMPARE(r.status, Status::Unsafe); QVERIFY(r.error.contains("no stage workspace"));
        ::chmod(root.toLocal8Bit().constData(), 0755); QVERIFY(submit("research", "spec", clean).error.contains("group/world")); ::chmod(root.toLocal8Bit().constData(), 0700);
        write(clean); ::chmod((root + "/research").toLocal8Bit().constData(), 0755);
        QVERIFY(HandoffStore::submit(root, "research", "spec", root + "/research/in.md").error.contains("group/world")); ::chmod((root + "/research").toLocal8Bit().constData(), 0700);
        QDir().mkdir(hand); ::chmod(hand.toLocal8Bit().constData(), 0777); QVERIFY(submit("research", "spec", clean).error.contains("group/world"));
    }
    void strayFileAndCorruptAudit() {
        QCOMPARE(submit("research", "spec", clean).status, Status::Released);
        put(hand + "/02-spec-to-impl.md", "x");
        auto r = submit("spec", "impl", clean); QCOMPARE(r.status, Status::Unsafe); QVERIFY(r.error.contains("stray file")); QCOMPARE(HandoffStore::verifyChain(root).exitCode, 1);
        QFile::remove(hand + "/02-spec-to-impl.md");
        setAuditLines(auditLines() << "not json");
        r = submit("spec", "impl", clean); QCOMPARE(r.status, Status::Unsafe); QVERIFY(r.error.contains("corrupt")); QCOMPARE(HandoffStore::verifyChain(root).exitCode, 1);
    }
    void failedReleaseLeavesNoFile() {
        QCOMPARE(submit("research", "spec", clean).status, Status::Released);
        QFile::remove(hand + "/MANIFEST.txt"); QVERIFY(QFile::link("/dev/null", hand + "/MANIFEST.txt"));
        const auto before = listing();
        QCOMPARE(submit("spec", "impl", clean).status, Status::Unsafe); QCOMPARE(listing(), before); QCOMPARE(audit().size(), 1);
    }
    void binaryAndInvalidUtf8() {
        auto r = HandoffStore::submit(root, "research", "spec", write(QByteArray("abc\0def", 7))); QCOMPARE(r.status, Status::Rejected); QVERIFY(rules(r).contains("R9"));
        r = HandoffStore::submit(root, "research", "spec", write("bad \xed\xa0\x80 surrogate\n", "research", "b.md")); QCOMPARE(r.status, Status::Rejected); QVERIFY(rules(r).contains("R9"));
        QCOMPARE(HandoffStore::submit(root, "research", "spec", write("nonchar \xef\xbf\xbe ok\n", "research", "c.md")).status, Status::Released);
    }
    void chainTamperDetected_data() {
        QTest::addColumn<QString>("what"); QTest::addColumn<QString>("message");
        QTest::newRow("modified release") << "modify" << "02-spec-to-impl.md was modified";
        QTest::newRow("deleted middle line") << "delmid" << "prev_hash mismatch";
        QTest::newRow("edited line") << "edit" << "prev_hash mismatch";
        QTest::newRow("deleted last release record") << "dellast" << "";
        QTest::newRow("orphan released file") << "orphan" << "no audit record";
        QTest::newRow("deleted released file") << "delfile" << "missing";
    }
    void chainTamperDetected() {
        QFETCH(QString, what); QFETCH(QString, message);
        threeReleases();
        QStringList lines = auditLines();
        if (what == "modify") { QFile f(hand + "/02-spec-to-impl.md"); f.open(QIODevice::Append); f.write("tampered\n"); }
        if (what == "delmid") { lines.removeAt(1); setAuditLines(lines); }
        if (what == "edit") { lines[0].replace("\"docs\"", "\"other\""); setAuditLines(lines); }
        if (what == "dellast") { lines.removeAt(2); setAuditLines(lines); }
        if (what == "orphan") {
            setAuditLines(lines.mid(0, 3)); QCOMPARE(HandoffStore::verifyChain(root).exitCode, 0); // dropping a trailing REJECT is a documented limit
            setAuditLines(lines.mid(0, 2));
        }
        if (what == "delfile") QFile::remove(hand + "/01-research-to-spec.md");
        const auto chain = HandoffStore::verifyChain(root);
        QCOMPARE(chain.exitCode, 1); QVERIFY2(chain.problems.join('\n').contains(message), qPrintable(chain.problems.join('\n')));
    }
    void provenance() {
        QCOMPARE(submit("research", "spec", "facts\n").status, Status::Released);
        const auto first = audit().last(); const QString n = first.value("output").toString(), h = first.value("output_sha256").toString();
        QCOMPARE(submit("spec", "impl", "spec\n").status, Status::Released);
        // impl cannot cite the research->spec release (its 'to' is spec, not impl)
        QVERIFY(rules(submit("impl", "validate", "note\n", {}, {n})).contains("P2"));
        QVERIFY(rules(submit("impl", "validate", ("Inputs: " + n + ' ' + h + '\n').toUtf8())).contains("P2"));
        for (const QString &bad : {"Inputs: " + n, "Inputs: " + h, "Inputs: " + n + ' ' + h.left(63), "Inputs: nosuch.md " + h, "Inputs: 05-research-to-spec.md " + h, "Inputs: " + n + ' ' + h + "; free text", "Inputs: " + n + ' ' + QString(64, '0')})
            QVERIFY2(rules(submit("spec", "impl", bad.toUtf8() + '\n')).contains("P2"), qPrintable(bad));
        for (const QString &ok : {"Inputs: " + n + ' ' + h, "Inputs: " + n + ' ' + h.toUpper()}) QCOMPARE(submit("spec", "impl", ok.toUtf8() + '\n').status, Status::Released);
        QCOMPARE(submit("research", "spec", "Inputs: research/trace/ (T0)\n").status, Status::Released);
        const QString p = write("x\n", "impl", "impl.md");
        QVERIFY(rules(HandoffStore::submit(root, "impl", "validate", p)).contains("P3"));
        QCOMPARE(HandoffStore::submit(root, "impl", "validate", write("Agent: h1\n", "impl", "h.md")).status, Status::Released);
        QVERIFY(rules(submit("spec", "impl", "Agent: a\n", "b")).contains("P1"));
        QVERIFY(rules(submit("spec", "impl", ("Inputs: 09-spec-to-impl.md " + h + '\n').toUtf8(), {}, {n})).contains("P1"));
        QCOMPARE(submit("spec", "impl", ("Agent: a\nInputs: " + n + ' ' + h + '\n').toUtf8(), "a", {n}).status, Status::Released);
        const auto r = audit().last(); QCOMPARE(r.value("agent").toString(), QString("a")); QCOMPARE(r.value("input_hashes").toArray(), QJsonArray({h}));
        // verify-chain re-checks Inputs hashes of released files
        QFile f(hand + '/' + r.value("output").toString()); f.open(QIODevice::ReadOnly); const QByteArray body = f.readAll().replace(h.toUtf8(), QByteArray(64, 'c')); f.close();
        put(f.fileName(), body);
        QVERIFY(HandoffStore::verifyChain(root).problems.join('\n').contains("matches no earlier released entry"));
    }
    void duplicateAndLeftoverTemp() {
        QCOMPARE(submit("research", "spec", "facts\n", {}, {}, false).status, Status::Released);
        QVERIFY(rules(submit("spec", "impl", "facts\n", {}, {}, false)).contains("D1"));
        put(hand + "/.tmp-02-spec-to-impl.md", "half");
        auto r = submit("spec", "impl", "next\n"); QCOMPARE(r.status, Status::Unsafe); QVERIFY(r.error.contains("leftover temp file")); QVERIFY(QFileInfo::exists(hand + "/.tmp-02-spec-to-impl.md"));
        QVERIFY(HandoffStore::verifyChain(root).problems.join('\n').contains("leftover temp file"));
    }
    void eventsAndSummary() {
        QCOMPARE(submit("spec", "impl", "see sub_1A2B\n").status, Status::Rejected);
        QCOMPARE(submit("research", "spec", "ok\n").status, Status::Released);
        QCOMPARE(HandoffStore::appendEvent(root, "TAINT", {{"stage", "spec"}, {"report_to_compliance", true}}), QString());
        for (const QString &d : {"spec.tainted/agent-a", "spec.tainted/agent-b", "impl.tainted", "notes.tainted.bak/x"}) QDir().mkpath(root + '/' + d);
        put(root + "/spec.tainted/file", "");
        const auto chain = HandoffStore::verifyChain(root);
        QCOMPARE(chain.exitCode, 0); QVERIFY(chain.warnings.join('\n').contains("impl.tainted has no <agent-id> subdirectory"));
        QVERIFY2(QRegularExpression("^records=3 last_hash=[0-9a-f]{64} tainted_stages=2 compliance_reports=2$").match(chain.summary).hasMatch(), qPrintable(chain.summary));
        QCOMPARE(HandoffStore::verifyChain(root, false).summary, chain.summary);
        QString error; QCOMPARE(HandoffStore::releases(root, &error).size(), 1); QVERIFY(error.isEmpty());
    }
    void pythonAndCppVerifyEachOther() {
        const QString python = QStandardPaths::findExecutable("python3");
        if (python.isEmpty()) QSKIP("python3 not found: cross-verification of audit chains with the python oracle was NOT checked");
        auto oracle = [&](const QStringList &args, QString *out = nullptr) {
            QProcess p; auto env = QProcessEnvironment::systemEnvironment(); env.insert("PYTHONDONTWRITEBYTECODE", "1"); p.setProcessEnvironment(env); p.start(python, QStringList{TEST_SOURCE_DIR "/tests/handoff-oracle/check_handoff.py"} + args); p.waitForFinished(60000);
            if (out) *out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
            return p.exitCode();
        };
        threeReleases();
        QCOMPARE(HandoffStore::appendEvent(root, "DELIVER", {{"stage", "validate"}, {"input", "02-spec-to-impl.md"}}), QString());
        QString pySummary; QCOMPARE(oracle({"--verify-chain", "--run-root", root}, &pySummary), 0);
        QCOMPARE(pySummary, HandoffStore::verifyChain(root).summary);
        // python extends the C++ chain; C++ verifies and extends it again; python verifies the result.
        const QString p = write("Agent: v1\nmismatch: behaviour differs\n", "validate", "v.md");
        QCOMPARE(oracle({"--run-root", root, "--from", "validate", "--to", "impl", "--file", p}), 0);
        QCOMPARE(HandoffStore::verifyChain(root).exitCode, 0);
        QCOMPARE(submit("impl", "validate", "second impl report\n").status, Status::Released);
        QCOMPARE(oracle({"--verify-chain", "--run-root", root}), 0);
        QStringList lines = auditLines(); lines[1].replace("\"spec\"", "\"xxxx\""); setAuditLines(lines);
        QCOMPARE(oracle({"--verify-chain", "--run-root", root}), 1);
        QCOMPARE(HandoffStore::verifyChain(root).exitCode, 1);
    }
};
QTEST_GUILESS_MAIN(HandoffStoreTest)
#include "handoff_store_test.moc"

// SPDX-License-Identifier: GPL-3.0-or-later
#include "handoffcheck.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
namespace {
const QString corpusDir = QStringLiteral(TEST_SOURCE_DIR "/tests/handoff-corpus");
QString readUtf8(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString(); }
QSet<QString> ids(const QList<HandoffCheck::Hit> &hits, bool eRules) {
    QSet<QString> out;
    for (const auto &h : hits) if (h.rule.startsWith('E') == eRules) out << h.rule;
    return out;
}
QSet<QString> column(const QString &value) {
    if (value == "-") return {};
    const QStringList parts = value.split(',');
    return QSet<QString>(parts.begin(), parts.end());
}
QString describe(const QList<HandoffCheck::Hit> &hits) {
    QStringList out;
    for (const auto &h : hits) out << QString("%1:%2 %3").arg(h.line).arg(h.rule, h.description);
    return out.join("; ");
}
}
class HandoffCheckTest : public QObject {
    Q_OBJECT
private slots:
    void corpus_data() {
        QTest::addColumn<QString>("file"); QTest::addColumn<QString>("oracle"); QTest::addColumn<QString>("extra");
        QFile manifest(corpusDir + "/expected.tsv"); QVERIFY(manifest.open(QIODevice::ReadOnly));
        int rows = 0;
        for (const QByteArray &raw : manifest.readAll().split('\n')) {
            const QStringList f = QString::fromUtf8(raw).split('\t');
            if (raw.startsWith('#') || f.size() != 3) continue;
            QTest::newRow(qPrintable(f[0])) << f[0] << f[1] << f[2]; ++rows;
        }
        QVERIFY(rows > 300);
    }
    // Table: every python test fixture plus adversarial encodings; the oracle column is the
    // oracle's own verdict, the extra column the launcher-only E rules expected on top.
    void corpus() {
        QFETCH(QString, file); QFETCH(QString, oracle); QFETCH(QString, extra);
        const QString text = readUtf8(corpusDir + '/' + file);
        const QList<HandoffCheck::Hit> recon = file.startsWith("recon-") ? HandoffCheck::reconFormat(text) : QList<HandoffCheck::Hit>();
        const auto plain = HandoffCheck::checkText(text) + recon, strict = HandoffCheck::checkTextStrict(text) + recon;
        QVERIFY2(ids(plain, false) == column(oracle), qPrintable(describe(plain)));
        QCOMPARE(ids(plain, true), QSet<QString>());
        QCOMPARE(ids(strict, false), column(oracle));
        if (extra != "*") QVERIFY2(ids(strict, true) == column(extra), qPrintable(describe(strict)));
    }
    void complianceRules() {
        QVERIFY(HandoffCheck::isComplianceRule({1, "R5", "x"}));
        QVERIFY(!HandoffCheck::isComplianceRule({1, "R4", "x"}));
        QVERIFY(HandoffCheck::isComplianceRule({1, "R10", "x"}));
        QVERIFY(!HandoffCheck::isComplianceRule({1, "F1", "x"}));
        QVERIFY(HandoffCheck::isComplianceRule({1, "E2", "x", "R1"}));
        QVERIFY(!HandoffCheck::isComplianceRule({1, "E3", "x", "R6"}));
        const auto hits = HandoffCheck::checkTextStrict(readUtf8(corpusDir + "/adv-base64-asm.txt"));
        QVERIFY(std::any_of(hits.begin(), hits.end(), HandoffCheck::isComplianceRule));
    }
    // Compliance-confirmed oracle version: drift of the vendored copy must fail loudly.
    void vendoredOracleIsTheConfirmedVersion() {
        const QHash<QString, QString> pinned{{"check_handoff.py", "4aafb1cdb9ee7563dcbf4c05169107bbfcf58c5ff41659dd6be71a7b6d6bf7c8"},
                                             {"test_check_handoff.py", "992f6c8b9851aaeda52ac898ae4ca150514fade815c56d81c320d0f20d7bdd46"}};
        for (auto it = pinned.begin(); it != pinned.end(); ++it) {
            QFile f(TEST_SOURCE_DIR "/tests/handoff-oracle/" + it.key()); QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex()), it.value());
        }
    }
    void longLineComplianceHitsSurviveTheWindowing() {
        const auto hits = HandoffCheck::checkTextStrict(QString("a ").repeated(100000) + "calls sub_1A2B\n");
        QVERIFY(std::any_of(hits.begin(), hits.end(), [](const auto &h) { return h.rule == "E4" && h.inner == "R5" && HandoffCheck::isComplianceRule(h); }));
        QVERIFY(!HandoffCheck::exposureMarkers(QString("x").repeated(20000) + " sub_1A2B\n").isEmpty()); // log scan past 4096 chars
    }
    void manyBase64BlobsStayFast() {
        QString text; for (int i = 0; i < 20000; ++i) text += "aGVsbG8gd29ybGQgZm9vYmFy\n"; // printable base64 on every line
        QElapsedTimer t; t.start();
        HandoffCheck::checkTextStrict(text);
        QVERIFY2(t.elapsed() < 5000, qPrintable(QString::number(t.elapsed())));
    }
    void headers() {
        const QString t = "Agent: res-1\nInputs: 01-a.md; 02-b.md\n";
        QCOMPARE(HandoffCheck::parseHeader(t, "Agent").value(), QString("res-1"));
        QCOMPARE(HandoffCheck::parseList(t, "Inputs"), QStringList({"01-a.md", "02-b.md"}));
        QVERIFY(!HandoffCheck::parseHeader("x\n", "Agent").has_value());
        QCOMPARE(HandoffCheck::parseHeader("**Agent:** a\n", "Agent").value(), QString("a"));
        QCOMPARE(HandoffCheck::parseHeader("Inputs:\n", "Inputs").value(), QString());
        QCOMPARE(HandoffCheck::parseSources("x\nSources: a b; c\n"), QStringList({"a b", "c"}));
        QCOMPARE(HandoffCheck::parseSources("x\nsource: d\n"), QStringList({"d"}));
        QCOMPARE(HandoffCheck::parseSources("none\n"), QStringList());
        QCOMPARE(HandoffCheck::parseHeader(QString::fromUtf8("\u0130nputs: x\n"), "Inputs").value(), QString("x")); // python re.I folds U+0130
        QCOMPARE(HandoffCheck::parseHeader(QString::fromUtf8("\u0130nputs: \u0130x\n"), "Inputs").value(), QString::fromUtf8("\u0130x")); // value keeps its text
    }
    void matchedTextNeverInDescriptions() {
        for (const auto &h : HandoffCheck::checkTextStrict("see sub_1234 here\n" + QString("ZWNobyBzdWJfMTIzNCBoZXJl") + "\n"))
            QVERIFY2(!h.description.contains("1234"), qPrintable(h.description));
    }
    // PCRE would report a match-limit error as "no match"; lines up to the E4 cap must still be matched.
    void longLinesMatchWithoutRegexErrors() {
        static QStringList warnings; warnings.clear();
        const auto previous = qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &m) { warnings << m; });
        const QString operands = "mov x0, " + QString(4000, ' ') + "x1";
        const auto hits = HandoffCheck::checkText(operands + '\n' + operands + '\n' + operands + '\n');
        const auto prose = HandoffCheck::checkTextStrict(QString("[x0, ").repeated(818) + '\n');
        qInstallMessageHandler(previous);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QVERIFY(std::any_of(hits.begin(), hits.end(), [](const auto &h) { return h.rule == "R1"; }));
        QVERIFY(std::none_of(prose.begin(), prose.end(), [](const auto &h) { return h.rule == "E4"; }));
        const auto tooLong = HandoffCheck::checkTextStrict(QString(HandoffCheck::MaxLineLength + 1, 'a'));
        QCOMPARE(tooLong.size(), 1); QCOMPARE(tooLong[0].rule, QString("E4")); QVERIFY(!HandoffCheck::isComplianceRule(tooLong[0]));
    }
    void exposureMarkersAreNarrow() {
        // Darling's own assembly and ordinary C in a log are not markers; decompiler names and dumps are.
        QVERIFY(HandoffCheck::exposureMarkers("mov x0, x1\nldr x2, [sp, #8]\nbl _foo\nint v1 = 2;\n__int64 x;\n").isEmpty());
        QCOMPARE(HandoffCheck::exposureMarkers("calls sub_1A2B\n").size(), 1);
        QCOMPARE(HandoffCheck::exposureMarkers("x\nContents of (__TEXT,__text) section\n").value(0).line, 2);
        QCOMPARE(HandoffCheck::exposureMarkers("calls \u0455ub_1A2B\n").size(), 1); // look-alike folded
        QVERIFY(HandoffCheck::exposureMarkers("ZWNobyBzdWJfMTIzNCBoZXJl\n").isEmpty()); // logs are not base64-decoded
        QCOMPARE(HandoffCheck::rawTraceObjects("{\"symbol\": \"_f\", \"x0\": 1}\n").size(), 1);
    }
    // R1 must cover the common AArch64 and x86 instruction families, not a fixed sample.
    void r1_data() {
        QTest::addColumn<QString>("text"); QTest::addColumn<bool>("flagged");
        auto block = [](const QStringList &l) { return l.join('\n') + '\n'; };
        QTest::newRow("a64-float") << block({"fadd d0, d1, d2", "fsub d3, d4, d5", "fmul d6, d7, d8"}) << true;
        QTest::newRow("a64-logic") << block({"bic x0, x1, x2", "mvn x3, x4", "neg x5, x6"}) << true;
        QTest::newRow("a64-ext") << block({"sxtw x0, w1", "uxtb w2, w3", "clz x4, x5", "rev x6, x7"}) << true;
        QTest::newRow("x86-shift") << block({"sar eax, 1", "not rbx", "xchg rax, rbx"}) << true;
        QTest::newRow("x86-jcc") << block({"jbe 0x10", "cmp eax, ebx", "sete al", "cmovne rax, rbx"}) << true;
        QTest::newRow("x86-bare") << block({"cdq", "syscall", "ret"}) << true;
        QTest::newRow("x86-att") << block({"addq $8, %rsp", "movl $1, %eax", "xorl %eax, %eax"}) << true;
        QTest::newRow("plain-b") << block({"b .L3", "b _foo", "b 0x1000"}) << true;
        QTest::newRow("armv7-branches") << block({"bx lr", "beq _label", "blx r3", "bne _loop"}) << true;
        QTest::newRow("armv7-imm") << block({"mov r0, #1", "mov r1, #2", "cmp r0, #0"}) << true;
        QTest::newRow("att-imm") << block({"movl $1, 4", "b #0x100", "movl $2, 8"}) << true;
        QTest::newRow("prose-arm32-branches") << block({"bne tomorrow", "beq today", "bge soon", "blt later"}) << false;
        QTest::newRow("sse") << block({"movss xmm0, xmm1", "movaps xmm2, xmm3", "pxor xmm4, xmm4"}) << true;
        QTest::newRow("semicolon-plus-prose") << QString("mov x0, x1; ldr x2, [sp, #8]; then we return; bl _foo\n") << true;
        QTest::newRow("pipe-joined") << QString("mov x0, x1 | ldr x2, [sp, #8] | bl _foo\n") << true;
        QTest::newRow("svc-imm") << block({"svc #0", "svc #0x80", "brk #1"}) << true;
        for (const char *op : {"cdq", "cqo", "cwde", "cdqe", "cltq", "cqto", "cltd", "cwtl", "cbtw", "syscall", "sysenter", "ud2", "eret", "wfi", "wfe", "leave", "nop", "hlt", "int3", "retq"})
            QTest::newRow(qPrintable(QString("bare-") + op)) << block({op, op, op}) << true;
        QTest::newRow("prose-loop") << block({"Loop forward/back/end", "Loop forward", "Loop end"}) << false;
        QTest::newRow("prose-bare-label-branches") << block({"- b main", "b 12", "js foo", "jo bar", "jc baz", "jp qux", "b label", "loop again"}) << false;
        QTest::newRow("prose-hash-numbers") << block({"fix #12", "rev #13", "neg #14", "not #15"}) << false;
        QTest::newRow("semicolon-joined") << QString("mov x0, x1; ldr x2, [sp, #8]; bl _foo\n") << true;
        QTest::newRow("semicolon-joined-x86") << QString("push rbp; mov rbp, rsp; sub rsp, 16\n") << true;
        QTest::newRow("prose-short-words") << block({"We not rev neg the sar value", "Option b is chosen; not c", "bic and mvn are words, as is clz"}) << false;
        QTest::newRow("prose-one-word-lines") << block({"not", "neg", "rev", "syscall is a word"}) << false;
        QTest::newRow("prose-b-sentence") << QString("b is a variable; call foo; push the button\n") << false;
        QTest::newRow("prose-numbers") << block({"neg 5", "not 3", "sar 2", "rev 1", "fadd 7"}) << false;
    }
    void r1() {
        QFETCH(QString, text); QFETCH(bool, flagged);
        for (bool strict : {false, true}) {
            const auto hits = strict ? HandoffCheck::checkTextStrict(text) : HandoffCheck::checkText(text);
            QVERIFY2(ids(hits, false).contains("R1") == flagged, qPrintable(describe(hits)));
        }
    }
    // R5 must cover Ghidra decompiler output as well as IDA; weak names need corroboration.
    void r5_data() {
        QTest::addColumn<QString>("text"); QTest::addColumn<bool>("flagged");
        QTest::newRow("FUN") << QString("calls FUN_00101a2b here") << true;
        QTest::newRow("PTR_FUN") << QString("x = PTR_FUN_00401000;") << true;
        QTest::newRow("_DAT") << QString("x = _DAT_100008000;") << true;
        QTest::newRow("thunk_FUN") << QString("thunk_FUN_00401000();") << true;
        QTest::newRow("DAT") << QString("x = DAT_00104000;") << true;
        QTest::newRow("CONCAT") << QString("y = CONCAT44(a, b);") << true;
        QTest::newRow("SUB84") << QString("y = SUB84(a, 0);") << true;
        QTest::newRow("ZEXT416") << QString("y = ZEXT416(a);") << true;
        QTest::newRow("in_stack") << QString("x = in_stack_ffffffe8;") << true;
        QTest::newRow("unaff") << QString("x = unaff_RBX;") << true;
        QTest::newRow("extraout") << QString("x = extraout_RAX;") << true;
        QTest::newRow("local_res") << QString("x = local_res8;") << true;
        QTest::newRow("undefined-plus-weak-block") << QString("undefined4 uVar1;\nuVar1 = param_1 + 4;") << true;
        QTest::newRow("undefined-weak-different-blocks") << QString("undefined4 x;\n\nuVar1 = param_1 + 4;") << false;
        QTest::newRow("python-weak-trio") << QString("def f(param_1):\n    local_1 = param_1\n    iVar1 = local_1\n") << false;
        QTest::newRow("spec-weak-pair") << QString("The spec names param_1 and envVar1 as inputs.\n") << false;
        QTest::newRow("undefined8-local") << QString("undefined8 foo(void)\nlocal_28 = 0;") << true;
        for (const char *t : {"envVar1", "newVar2", "param_1", "param_2", "local_cafe", "local_1", "DAT_1234", "undefined2", "in_RDI"})
            QTest::newRow(qPrintable(QString("weak-alone-") + t)) << QString("see %1 here").arg(t) << false;
        QTest::newRow("near-prose") << QString("The function is undefined, the local variable param is unnamed; a data file DAT holds it.\n") << false;
        QTest::newRow("near-identifiers") << QString("local_name and param_x and funny_ones and variable1\n") << false;
    }
    void r5() {
        QFETCH(QString, text); QFETCH(bool, flagged);
        for (bool strict : {false, true}) {
            const auto hits = strict ? HandoffCheck::checkTextStrict(text) : HandoffCheck::checkText(text);
            QVERIFY2(ids(hits, false).contains("R5") == flagged, qPrintable(describe(hits)));
        }
    }
    void exposureMarkersGhidra() {
        for (const char *t : {"FUN_00101a2b called", "x = _DAT_100008000;", "CONCAT44(a, b)", "undefined8 f()\nlocal_28 = 0;"})
            QVERIFY2(!HandoffCheck::exposureMarkers(t).isEmpty(), t);
        QVERIFY(HandoffCheck::exposureMarkers("The function is undefined; local variable param.\n").isEmpty());
        QVERIFY(HandoffCheck::exposureMarkers("iVar1 = foo();\n").isEmpty());
        QVERIFY(HandoffCheck::exposureMarkers("see param_1 here\n").isEmpty());
        QVERIFY(HandoffCheck::exposureMarkers("see local_1 here\n").isEmpty());
    }
    // C++ vs python on the same corpus, plus mechanical variants of it, so the port cannot drift.
    void parityWithPythonOracle() {
        const QString python = QStandardPaths::findExecutable("python3");
        if (python.isEmpty()) QSKIP("python3 not found: C++/python parity of the handoff checker was NOT checked");
        QTemporaryDir variants; QVERIFY(variants.isValid());
        const QStringList files = QDir(corpusDir).entryList({"*.txt"}, QDir::Files);
        const QList<std::function<QString(const QString &)>> mutations{
            [](const QString &s) { return s; },
            [](const QString &s) { return s.toUpper(); },
            [](const QString &s) { QString o; for (const QString &l : s.split('\n')) o += "> " + l + "\r\n"; return o; },
            [](const QString &s) { QString o = s; return o.replace(' ', QChar(0x00a0)); },
            [](const QString &s) { QString o; for (const QString &l : s.split('\n')) o += "- `" + l + "`\n"; return o; },
        };
        for (const QString &f : files)
            for (int m = 0; m < mutations.size(); ++m) {
                QFile out(variants.path() + QString("/%1-%2").arg(m).arg(f)); QVERIFY(out.open(QIODevice::WriteOnly));
                out.write(mutations[m](readUtf8(corpusDir + '/' + f)).toUtf8());
            }
        QProcess oracle; auto env = QProcessEnvironment::systemEnvironment(); env.insert("PYTHONDONTWRITEBYTECODE", "1"); oracle.setProcessEnvironment(env);
        oracle.start(python, {TEST_SOURCE_DIR "/tests/handoff_parity.py", TEST_SOURCE_DIR "/tests/handoff-oracle", variants.path()});
        QVERIFY(oracle.waitForFinished(120000)); QCOMPARE(oracle.exitCode(), 0);
        const QJsonObject expected = QJsonDocument::fromJson(oracle.readAllStandardOutput()).object();
        QCOMPARE(expected.size(), files.size() * int(mutations.size()));
        int mismatches = 0;
        for (auto it = expected.begin(); it != expected.end(); ++it) {
            QList<HandoffCheck::Hit> want;
            for (const auto &h : it.value().toArray()) want << HandoffCheck::Hit{h[0].toInt(), h[1].toString(), h[2].toString()};
            const QString text = readUtf8(variants.path() + '/' + it.key());
            auto got = HandoffCheck::checkText(text);
            if (it.key().contains("recon-")) { got += HandoffCheck::reconFormat(text); std::sort(got.begin(), got.end()); }
            if (got != want) { ++mismatches; qWarning().noquote() << it.key() << "python:" << describe(want) << "c++:" << describe(got); }
        }
        QCOMPARE(mismatches, 0);
    }
};
QTEST_GUILESS_MAIN(HandoffCheckTest)
#include "handoff_check_test.moc"

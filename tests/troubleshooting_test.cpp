// SPDX-License-Identifier: GPL-3.0-or-later
#include "troubleshooting.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QtTest>
class TroubleshootingTest : public QObject {
    Q_OBJECT
private slots:
    void troubleshootingApprovalBoundaries() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const auto previousPath = qgetenv("PATH"); const auto restore = qScopeGuard([=] { qputenv("PATH", previousPath); });
        auto write = [&](const QString &name, const QByteArray &contents) { QFile file(temporary.path() + '/' + name); if (!file.open(QIODevice::WriteOnly)) return false; file.write(contents); file.close(); return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); };
        QVERIFY(write("gh", "#!/bin/sh\nprintf '%s\\n' \"$*\" >> '" + temporary.path().toUtf8() + "/gh-calls'\ncase \"$1\" in auth) exit 0;; api) printf 'reviewed-sha\\n';; pr) printf 'https://example.invalid/pull/1\\n';; esac\n"));
        QVERIFY(write("git", "#!/bin/sh\ncase \"$3\" in status) exit 0;; symbolic-ref) echo fix-fixture;; rev-parse) echo reviewed-sha;; diff) if [ \"$4\" = --numstat ]; then printf '1\\t0\\tfixture.cpp\\n'; else printf 'diff --git a/fixture.cpp b/fixture.cpp\\n+source fix\\n'; fi;; esac\n"));
        QVERIFY(write("claude", "#!/bin/sh\nexit 0\n")); QVERIFY(write("codex", "#!/bin/sh\nexit 0\n")); QVERIFY(write("opencode", "#!/bin/sh\nexit 0\n"));
        QVERIFY(write("xdg-terminal-exec", "#!/bin/sh\nprintf '%s\\n' \"$*\" > '" + temporary.path().toUtf8() + "/agent-calls'\n"));
        qputenv("PATH", temporary.path().toUtf8());
        QCOMPARE(LauncherTroubleshooting::agents(), (QStringList{"claude", "codex", "opencode"}));
        QVERIFY(LauncherTroubleshooting::agentArguments("codex", "prompt").contains("gpt-6.1-sol")); QVERIFY(LauncherTroubleshooting::agentArguments("unknown", "prompt").isEmpty());
        const QString prefix = temporary.path() + "/prefix", source = temporary.path() + "/volume"; QVERIFY(QDir().mkpath(prefix)); QVERIFY(QDir().mkpath(source));
        const QJsonObject data{{"app", "Fixture"}, {"prefix", prefix}, {"sourceVolume", source}, {"loaderOutput", "Library not loaded: /usr/lib/fixture"}};
        TroubleshootingDialog dialog(data); dialog.show(); auto *start = dialog.findChild<QPushButton *>("startFixAgent"); QVERIFY(!start->isEnabled());
        QCOMPARE(QJsonDocument::fromJson(dialog.findChild<QTextEdit *>("troubleshootingData")->toPlainText().toUtf8()).object(), data);
        QVERIFY(dialog.findChild<QLabel *>("contributionChoices")->text().contains("Local workaround"));
        auto *review = dialog.findChild<QPushButton *>("reviewPrProposal"); QTRY_VERIFY(review->isEnabled());
        dialog.findChild<QCheckBox *>("approveAgentData")->setChecked(true); QVERIFY(start->isEnabled());
        const QString workspace = temporary.path() + "/work";
        dialog.findChild<QLineEdit *>("agentWorkspace")->setText(prefix + "/forbidden"); start->click(); QVERIFY(!QFileInfo::exists(prefix + "/forbidden"));
        QVERIFY(QFile::link(prefix, temporary.path() + "/alias")); dialog.findChild<QLineEdit *>("agentWorkspace")->setText(temporary.path() + "/alias/forbidden"); start->click(); QVERIFY(!QFileInfo::exists(prefix + "/forbidden"));
        dialog.findChild<QLineEdit *>("agentWorkspace")->setText(workspace); start->click(); QTRY_VERIFY(QFileInfo::exists(temporary.path() + "/agent-calls"));
        QFile report(workspace + "/TROUBLESHOOTING.json"); QVERIFY(report.open(QIODevice::ReadOnly)); QCOMPARE(QJsonDocument::fromJson(report.readAll()).object(), data);
        QVERIFY(QDir().mkpath(workspace + "/clone/.git"));
        const QJsonObject proposal{{"source", workspace + "/clone"}, {"repo", "VibeDarling/fixture"}, {"base", "main"}, {"head", "test:fix-fixture"}, {"title", "fixture: repair missing behavior"}, {"body", "Completed synthetic proposal"}};
        QFile proposalFile(workspace + "/proposal.json"); QVERIFY(proposalFile.open(QIODevice::WriteOnly)); proposalFile.write(QJsonDocument(proposal).toJson()); proposalFile.close();
        QVERIFY2(LauncherTroubleshooting::reviewProposal(proposalFile.fileName()).valid(), qPrintable(LauncherTroubleshooting::reviewProposal(proposalFile.fileName()).error));
        auto *timer = new QTimer(&dialog); timer->setInterval(10); bool approved = false;
        connect(timer, &QTimer::timeout, &dialog, [&] { if (auto *approval = dialog.findChild<QDialog *>("prApprovalDialog")) { timer->stop(); if (approved) approval->findChild<QPushButton *>("approveCompletedPr")->click(); else approval->reject(); } });
        timer->start(); dialog.loadProposal(proposalFile.fileName()); QTRY_VERIFY(!timer->isActive());
        QFile calls(temporary.path() + "/gh-calls"); QVERIFY(calls.open(QIODevice::ReadOnly)); QVERIFY(!calls.readAll().contains("pr create")); calls.close();
        approved = true; timer->start(); dialog.loadProposal(proposalFile.fileName()); QTRY_VERIFY(dialog.findChild<QLabel *>("troubleshootingStatus")->text().startsWith("PR submitted:"));
        QVERIFY(calls.open(QIODevice::ReadOnly)); const auto submitted = calls.readAll(); QVERIFY(submitted.contains("pr create --repo VibeDarling/fixture")); QVERIFY(submitted.contains("--body-file"));
        calls.close();
        QVERIFY(write("gh", "#!/bin/sh\nprintf '%s\\n' \"$*\" >> '" + temporary.path().toUtf8() + "/gh-calls'\ncase \"$1\" in auth) exit 0;; api) echo different-sha;; pr) echo unexpected;; esac\n"));
        timer->start(); dialog.loadProposal(proposalFile.fileName()); QTRY_VERIFY(dialog.findChild<QLabel *>("troubleshootingStatus")->text().contains("No PR submitted"));
        QVERIFY(calls.open(QIODevice::ReadOnly)); QCOMPARE(calls.readAll().count("pr create"), 1); calls.close();
        QVERIFY(write("git", "#!/bin/sh\ncase \"$3\" in status) exit 0;; symbolic-ref) echo fix-fixture;; rev-parse) echo reviewed-sha;; diff) printf -- '-\\t-\\tApple.app\\n';; esac\n"));
        QVERIFY(!LauncherTroubleshooting::reviewProposal(proposalFile.fileName()).valid());
        QVERIFY(write("gh", "#!/bin/sh\nexit 1\n"));
        TroubleshootingDialog unauthenticated(data); unauthenticated.show();
        QTRY_VERIFY(unauthenticated.findChild<QLabel *>("troubleshootingStatus")->text().contains("not authenticated"));
        QVERIFY(!unauthenticated.findChild<QPushButton *>("reviewPrProposal")->isEnabled());
        unauthenticated.loadProposal(proposalFile.fileName()); QVERIFY(unauthenticated.findChild<QLabel *>("troubleshootingStatus")->text().contains("Authenticate"));
    }
};
QTEST_MAIN(TroubleshootingTest)
#include "troubleshooting_test.moc"

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
#include <QPointer>
#include <QtTest>
class TroubleshootingTest : public QObject {
    Q_OBJECT
private slots:
    void rememberedLaunchChoicesAndMountOffer() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
        LauncherTroubleshooting::saveRecoveryChoices({true, true, false, true, "codex"});
        LaunchChoicesDialog choices("Fixture", false); choices.show();
        QVERIFY(!choices.findChild<QCheckBox *>("launchImportOption")->isVisible());
        QSignalSpy mount(&choices, &LaunchChoicesDialog::mountRequested); choices.findChild<QPushButton *>("launchMountSource")->click(); QCOMPARE(mount.count(), 1);
        QVERIFY(choices.findChild<QCheckBox *>("rememberLaunchChoices")->isChecked()); QVERIFY(choices.choices().report);
        choices.show(); choices.setMounted(true); QVERIFY(choices.findChild<QCheckBox *>("launchImportOption")->isVisible()); QVERIFY(!choices.findChild<QPushButton *>("launchMountSource")->isVisible());
        choices.findChild<QCheckBox *>("rememberLaunchChoices")->setChecked(false); LauncherTroubleshooting::saveRecoveryChoices(choices.choices()); QVERIFY(!LauncherTroubleshooting::recoveryChoices().remember);
    }

    void mountOfferTracksAvailability() {
        QJsonObject data{{"app", "Fixture"}, {"missingLibrary", "/usr/lib/fixture.dylib"}};
        TroubleshootingDialog popup(data); popup.show();
        auto *copy = popup.findChild<QCheckBox *>("importDependencyOption");
        auto *mount = popup.findChild<QPushButton *>("mountDependencySource");
        QVERIFY(!copy->isVisible()); QVERIFY(mount->isVisible());
        QSignalSpy requested(&popup, &TroubleshootingDialog::mountRequested); mount->click(); QCOMPARE(requested.count(), 1);
        data.insert("sourceMounted", true); popup.updateDiagnostic(data, true, false);
        QVERIFY(copy->isVisible()); QVERIFY(copy->isEnabled()); QVERIFY(!mount->isVisible());
        copy->setChecked(true); data.insert("sourceMounted", false); popup.updateDiagnostic(data, false, false);
        QVERIFY(!copy->isVisible()); QVERIFY(!copy->isChecked()); QVERIFY(mount->isVisible());
    }

    void missingDependencyChoicesAndBackgroundLifetime() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const auto oldPath = qgetenv("PATH"); auto restore = qScopeGuard([=] { qputenv("PATH", oldPath); });
        auto write = [&](const QString &name, const QByteArray &contents) { QFile file(temporary.path() + '/' + name); if (!file.open(QIODevice::WriteOnly)) return false; file.write(contents); file.close(); return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); };
        QVERIFY(write("codex", "#!/bin/sh\nprintf '%s\\n' \"$*\" > '" + temporary.path().toUtf8() + "/agent-calls'\nprintf 'working\\n'\n/usr/bin/sleep 1\nprintf 'finished\\n'\n"));
        QVERIFY(write("gh", "#!/bin/sh\nprintf '%s\\n' \"$*\" >> '" + temporary.path().toUtf8() + "/gh-calls'\nif [ \"$1\" = issue ]; then while [ \"$#\" -gt 0 ]; do if [ \"$1\" = --body-file ]; then /usr/bin/cat \"$2\" > '" + temporary.path().toUtf8() + "/approved-body'; fi; shift; done; echo https://example.invalid/issues/1; fi\n"));
        qputenv("PATH", temporary.path().toUtf8());
        QVERIFY(LauncherTroubleshooting::backgroundArguments("codex", "test").contains("workspace-write")); QCOMPARE(LauncherTroubleshooting::backgroundArguments("claude", "test").first(), "--print"); QCOMPARE(LauncherTroubleshooting::backgroundArguments("opencode", "test").first(), "run"); QVERIFY(LauncherTroubleshooting::backgroundArguments("unknown", "test").isEmpty());
        QWidget owner; owner.show();
        const QString prefix = temporary.path() + "/prefix", source = temporary.path() + "/source"; QVERIFY(QDir().mkpath(prefix)); QVERIFY(QDir().mkpath(source));
        QJsonObject data{{"app", "Fixture"}, {"bundle", "Applications/Fixture.app"}, {"executable", "Fixture"}, {"sourceBundle", "Applications/Fixture.app"}, {"prefix", prefix}, {"sourceVolume", source}, {"launcher", "/fake/darling"}, {"missingLibrary", "/usr/lib/fixture.dylib"}, {"missingSymbol", "_Fixture"}, {"sourceMounted", true}, {"loaderOutput", "Symbol not found: _Fixture"}};
        auto *popup = new TroubleshootingDialog(data, &owner); popup->setAttribute(Qt::WA_DeleteOnClose); popup->show(); popup->updateDiagnostic(data, true, false);
        auto *copy = popup->findChild<QCheckBox *>("importDependencyOption"), *report = popup->findChild<QCheckBox *>("reportIssueOption"), *ai = popup->findChild<QCheckBox *>("aiFixOption"), *consent = popup->findChild<QCheckBox *>("approveAgentData");
        QVERIFY(copy && report && ai); QVERIFY(!copy->isChecked() && !report->isChecked() && !ai->isChecked());
        QVERIFY(!popup->findChild<QTextEdit *>("troubleshootingData")->isVisible()); popup->findChild<QPushButton *>("showDependencyDetails")->click(); QVERIFY(popup->findChild<QTextEdit *>("troubleshootingData")->isVisible());
        auto *apply = popup->findChild<QPushButton *>("runDependencyActions"); QVERIFY(!apply->isEnabled()); copy->setChecked(true); report->setChecked(true); ai->setChecked(true); QVERIFY(!apply->isEnabled()); consent->setChecked(true); QVERIFY(apply->isEnabled());
        data.insert("loaderOutput", "Symbol not found: _Fixture\nExpected in: /usr/lib/fixture.dylib"); popup->updateDiagnostic(data, true, false); QVERIFY(!consent->isChecked()); QVERIFY(!apply->isEnabled()); consent->setChecked(true);
        popup->findChild<QLineEdit *>("agentWorkspace")->setText(temporary.path() + "/work"); QTRY_VERIFY(popup->findChild<QPushButton *>("reviewPrProposal")->isEnabled());
        QSignalSpy imports(popup, &TroubleshootingDialog::importRequested); apply->click(); QCOMPARE(imports.count(), 1);
        QTRY_VERIFY(QFileInfo::exists(temporary.path() + "/agent-calls")); QCOMPARE(owner.findChildren<BackgroundFix *>().size(), 1);
        auto *job = owner.findChild<BackgroundFix *>(); QVERIFY(job->isRunning());
        QFile calls(temporary.path() + "/gh-calls"); QVERIFY(calls.open(QIODevice::ReadOnly)); QVERIFY(!calls.readAll().contains("issue create")); calls.close();
        auto *issue = popup->findChild<QDialog *>("issueApprovalDialog"); QVERIFY(issue); QVERIFY(issue->findChild<QTextEdit *>("issueBody")->toPlainText().contains("DPREFIX=")); issue->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        popup->reviewIssue(); issue = popup->findChild<QDialog *>("issueApprovalDialog"); QVERIFY(issue);
        issue->findChild<QLineEdit *>("issueRepository")->setText("wrong/repository"); issue->findChild<QPushButton *>("approveCompletedIssue")->click(); QVERIFY(issue->findChild<QLabel *>("issueSubmissionStatus")->text().contains("Choose a VibeDarling"));
        issue->findChild<QLineEdit *>("issueRepository")->setText("VibeDarling/fixture"); const QString approved = "Exact reviewed draft\nLiteral $(echo secret) and `literal` remain text.\n"; issue->findChild<QTextEdit *>("issueBody")->setPlainText(approved); issue->findChild<QPushButton *>("approveCompletedIssue")->click();
        QTRY_VERIFY(issue->findChild<QLabel *>("issueSubmissionStatus")->text().startsWith("Issue submitted:"));
        QFile body(temporary.path() + "/approved-body"); QVERIFY(body.open(QIODevice::ReadOnly)); QCOMPARE(QString::fromUtf8(body.readAll()), approved);
        QPointer<TroubleshootingDialog> remaining(popup); popup->reject(); QTRY_VERIFY(remaining.isNull()); QVERIFY(job->isRunning()); QTRY_VERIFY(!job->isRunning());
        QFile log(temporary.path() + "/work/AGENT.log"); QVERIFY(log.open(QIODevice::ReadOnly)); QVERIFY(log.readAll().contains("finished"));
        QVERIFY(QFile::remove(temporary.path() + "/codex")); TroubleshootingDialog noTools(data); QVERIFY(!noTools.findChild<QCheckBox *>("aiFixOption")->isEnabled()); QVERIFY(!noTools.findChild<QCheckBox *>("importDependencyOption")->isEnabled());
    }

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

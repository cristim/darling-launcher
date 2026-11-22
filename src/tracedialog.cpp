// SPDX-License-Identifier: GPL-3.0-or-later
#include "tracedialog.h"
#include "trace.h"
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QUrl>
#include <QHeaderView>
#include <QVBoxLayout>
#include <memory>

void showTraceLog(QWidget *parent, const QString &prefix) {
    auto *dialog = new QDialog(parent); dialog->setObjectName("traceLogDialog"); dialog->setWindowTitle("Missing-symbol trace log"); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->resize(720, 420);
    auto *layout = new QVBoxLayout(dialog);
    const QString directory = LauncherTrace::traceDirectory(prefix);
    auto *files = new QComboBox; files->setObjectName("traceFiles");
    for (const auto &info : QDir(directory).entryInfoList({"*.jsonl"}, QDir::Files | QDir::NoSymLinks, QDir::Time)) files->addItem(info.fileName(), info.absoluteFilePath());
    auto *note = new QLabel; note->setObjectName("traceNote"); note->setWordWrap(true);
    auto *table = new QTableWidget(0, 4); table->setObjectName("traceTable"); table->setHorizontalHeaderLabels({"Symbol", "Library", "Referencing image", "Kind"}); table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->horizontalHeader()->setStretchLastSection(true);
    auto summary = std::make_shared<LauncherTrace::Summary>();
    auto load = [=] {
        *summary = LauncherTrace::parse(files->currentData().toString());
        table->setRowCount(summary->rows.size());
        for (int i = 0; i < summary->rows.size(); ++i) {
            const auto &row = summary->rows.at(i);
            const QString kind = row.reason.isEmpty() ? row.kind : row.kind + " (" + row.reason + ")";
            table->setItem(i, 0, new QTableWidgetItem(row.symbol)); table->setItem(i, 1, new QTableWidgetItem(row.library));
            table->setItem(i, 2, new QTableWidgetItem(row.from)); table->setItem(i, 3, new QTableWidgetItem(kind));
        }
        QStringList parts; if (summary->unreadable) parts << "Cannot read this file"; if (files->count() == 0) parts << "No trace logs in " + directory;
        if (summary->skipped > 0) parts << QString::number(summary->skipped) + " lines skipped";
        if (summary->truncated) parts << "File larger than the read limit; only the beginning is shown";
        note->setText(parts.join(". "));
    };
    auto *buttons = new QHBoxLayout; auto *open = new QPushButton("Open trace log folder"); open->setObjectName("openTraceLog"); auto *save = new QPushButton("Export summary"); save->setObjectName("exportTraceSummary");
    buttons->addWidget(open); buttons->addWidget(save); buttons->addStretch();
    QObject::connect(open, &QPushButton::clicked, dialog, [directory] { QDesktopServices::openUrl(QUrl::fromLocalFile(directory)); });
    QObject::connect(save, &QPushButton::clicked, dialog, [=] {
        QString path, error;
        note->setText(LauncherTrace::writeSummary(prefix, *summary, &path, &error) ? "Summary (names only) saved to " + path : error);
    });
    QObject::connect(files, &QComboBox::currentIndexChanged, dialog, load);
    layout->addWidget(files); layout->addWidget(note); layout->addWidget(table); layout->addLayout(buttons);
    load(); dialog->show();
}

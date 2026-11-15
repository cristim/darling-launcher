// SPDX-License-Identifier: GPL-3.0-or-later
#include "appneedsdialog.h"
#include "automount.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

using FrameworkResources::formatBytes;

AppNeedsDialog::AppNeedsDialog(const AppNeeds::Plan &plan, QWidget *parent) : QDialog(parent), current(plan) {
    setObjectName("appNeedsDialog"); setWindowTitle("Framework resources for " + plan.appName); resize(640, 460);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel("<b>" + plan.appName.toHtmlEscaped() + "</b> links these macOS frameworks whose resource files (nibs, strings, data models, "
                             "Info.plist) are not in the prefix. Only resource files are copied, never executable code. Nothing is copied until you choose Import.");
    intro->setWordWrap(true); layout->addWidget(intro);
    table = new QTableWidget(0, 4); table->setObjectName("needsTable");
    table->setHorizontalHeaderLabels({"Framework", "Files", "On disk", "Why"});
    table->horizontalHeader()->setStretchLastSection(true); table->verticalHeader()->hide(); table->setSelectionMode(QAbstractItemView::NoSelection);
    for (int i = 0; i < current.items.size(); ++i) {
        const AppNeeds::Item &item = current.items[i];
        if (item.status == AppNeeds::Status::Skipped) continue;
        const int row = table->rowCount(); table->insertRow(row); rowItem << i;
        auto *name = new QTableWidgetItem(item.name); name->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        name->setCheckState(item.status == AppNeeds::Status::Selected ? Qt::Checked : Qt::Unchecked);
        table->setItem(row, 0, name);
        table->setItem(row, 1, new QTableWidgetItem(QString::number(item.files)));
        table->setItem(row, 2, new QTableWidgetItem(formatBytes(item.bytes)));
        table->setItem(row, 3, new QTableWidgetItem(item.reason));
    }
    layout->addWidget(table, 1);
    const auto skipped = current.byStatus(AppNeeds::Status::Skipped);
    auto *skippedBox = new QGroupBox(QString("Skipped frameworks (%1)").arg(skipped.size())); skippedBox->setObjectName("needsSkippedBox");
    skippedBox->setCheckable(true); skippedBox->setChecked(false);
    auto *skippedLayout = new QVBoxLayout(skippedBox);
    auto *skippedText = new QPlainTextEdit; skippedText->setObjectName("needsSkipped"); skippedText->setReadOnly(true); skippedText->setMaximumHeight(110);
    QStringList lines; for (const AppNeeds::Item *it : skipped) lines << it->name + ": " + it->reason;
    skippedText->setPlainText(lines.join('\n')); skippedLayout->addWidget(skippedText);
    skippedText->setVisible(false);
    connect(skippedBox, &QGroupBox::toggled, skippedText, &QWidget::setVisible);
    layout->addWidget(skippedBox);
    totals = new QLabel; totals->setObjectName("needsTotals"); layout->addWidget(totals);
    auto *buttons = new QDialogButtonBox;
    importButton = buttons->addButton("Import selected resources", QDialogButtonBox::AcceptRole); importButton->setObjectName("needsImport");
    auto *skip = buttons->addButton("Skip", QDialogButtonBox::RejectRole); skip->setObjectName("needsSkip");
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    importButton->setAutoDefault(false); skip->setDefault(true); // a stray Enter must not start a copy
    layout->addWidget(buttons);
    connect(table, &QTableWidget::itemChanged, this, [this] { update(); });
    update();
}

void AppNeedsDialog::update() {
    for (int row = 0; row < rowItem.size(); ++row)
        current.items[rowItem[row]].status = table->item(row, 0)->checkState() == Qt::Checked ? AppNeeds::Status::Selected : AppNeeds::Status::Optional;
    const int count = current.selectedCount();
    totals->setText(QString("Selected: %1 frameworks, %2 files, about %3 on disk. Free space on the prefix: %4.").arg(count).arg(current.selectedFiles())
                        .arg(formatBytes(current.selectedBytes()), current.availableBytes < 0 ? QString("unknown") : formatBytes(quint64(current.availableBytes))));
    importButton->setEnabled(count > 0);
}
AppNeeds::Plan AppNeedsDialog::plan() const { return current; }

CacheOfferDialog::CacheOfferDialog(const AppNeedsCache::Preview &preview, QWidget *parent) : QDialog(parent) {
    setObjectName("cacheOfferDialog"); setWindowTitle("dyld shared cache"); resize(620, 480);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel("This app is served by the macOS dyld shared cache. Copying it is a separate step from the framework resources, "
                             "and it is optional.");
    intro->setWordWrap(true); layout->addWidget(intro);
    auto *text = new QTextEdit; text->setObjectName("cacheText"); text->setReadOnly(true); text->setPlainText(AppNeedsCache::summarize(preview)); layout->addWidget(text, 1);
    auto *accept = new QCheckBox("I understand these are Apple executable binaries copied from my own macOS install, kept on this machine and never redistributed");
    accept->setObjectName("cacheAccept"); layout->addWidget(accept);
    auto *buttons = new QDialogButtonBox;
    auto *copy = buttons->addButton("Copy the cache", QDialogButtonBox::AcceptRole); copy->setObjectName("cacheCopy");
    auto *skip = buttons->addButton("Not now", QDialogButtonBox::RejectRole); skip->setObjectName("cacheSkip");
    const bool offerable = preview.cacheApp && preview.blocker.isEmpty() && !preview.alreadyPresent();
    copy->setEnabled(false); accept->setEnabled(offerable);
    connect(accept, &QCheckBox::toggled, copy, [copy, offerable](bool on) { copy->setEnabled(on && offerable); });
    if (preview.blocker.contains("not mounted")) {
        auto *mount = buttons->addButton("Mount the macOS cryptex image…", QDialogButtonBox::ActionRole); mount->setObjectName("cacheMount");
        connect(mount, &QPushButton::clicked, this, [this] { emit mountRequested(); });
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

MountExplainDialog::MountExplainDialog(const AutoMount::Need &need, const QList<MacPartition> &parts, QWidget *parent) : QDialog(parent), partitions(parts) {
    setObjectName("mountExplainDialog"); setWindowTitle("Administrator rights are needed to read your macOS volumes"); resize(600, 420);
    auto *layout = new QVBoxLayout(this);
    if (partitions.size() > 1) {
        layout->addWidget(new QLabel("Several APFS partitions were found. Choose the one that holds macOS:"));
        choice = new QComboBox; choice->setObjectName("mountPartition");
        for (const MacPartition &p : partitions) choice->addItem(AutoMount::partitionLine(p));
        layout->addWidget(choice);
    }
    auto *text = new QTextEdit; text->setObjectName("mountExplanation"); text->setReadOnly(true);
    text->setPlainText(AutoMount::explanation(need, partitions.isEmpty() ? MacPartition{} : partitions.first()));
    if (choice) connect(choice, &QComboBox::currentIndexChanged, text, [this, text, need](int i) { text->setPlainText(AutoMount::explanation(need, partitions.value(i))); });
    layout->addWidget(text, 1);
    auto *buttons = new QDialogButtonBox;
    auto *go = buttons->addButton("Continue", QDialogButtonBox::AcceptRole); go->setObjectName("mountContinue");
    auto *cancel = buttons->addButton("Cancel", QDialogButtonBox::RejectRole); cancel->setObjectName("mountCancel");
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
MacPartition MountExplainDialog::chosen() const { return partitions.value(choice ? choice->currentIndex() : 0); }

// SPDX-License-Identifier: GPL-3.0-or-later
#include "appbrowser.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMimeData>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QStyle>

namespace {
const QString bundleMime = "application/x-darling-app-bundles";
enum Roles { NameRole = Qt::UserRole + 1, SizeRole };
class AppDelegate : public QStyledItemDelegate {
public:
    explicit AppDelegate(AppBrowser *browser) : QStyledItemDelegate(browser), browser(browser) {}
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (browser->viewMode() == QListView::ListMode)
            option->text = index.data(NameRole).toString() + "    " + QLocale().formattedDataSize(index.data(SizeRole).toLongLong(), 1, QLocale::DataSizeSIFormat);
    }
private:
    AppBrowser *browser;
};
}
AppBrowser::AppBrowser(QWidget *parent) : QListWidget(parent) {
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragEnabled(true); setDragDropMode(QAbstractItemView::DragOnly); setDefaultDropAction(Qt::CopyAction);
    setMovement(QListView::Static); setResizeMode(QListView::Adjust);
    setItemDelegate(new AppDelegate(this)); setGridView(false);
}
void AppBrowser::setGridView(bool grid) {
    setViewMode(grid ? QListView::IconMode : QListView::ListMode);
    setIconSize(grid ? QSize(64, 64) : QSize(32, 32));
    setGridSize(grid ? QSize(128, 112) : QSize());
    setWordWrap(grid); setSpacing(grid ? 8 : 2); setWrapping(grid);
    setMovement(QListView::Static); setResizeMode(QListView::Adjust);
    viewport()->update();
}
void AppBrowser::showPreviews(const QList<AppPreview> &previews) {
    clear();
    for (const auto &preview : previews) {
        QImageReader reader(preview.iconPath);
        QImage image;
        if (!preview.iconPath.isEmpty()) image = reader.read();
        QIcon icon = image.isNull() ? style()->standardIcon(QStyle::SP_FileIcon) : QIcon(QPixmap::fromImage(image));
        auto *item = new QListWidgetItem(icon, preview.name, this);
        item->setData(Qt::UserRole, preview.relativeBundle); item->setData(NameRole, preview.name); item->setData(SizeRole, QVariant::fromValue(preview.bytes));
        item->setToolTip(preview.relativeBundle + "\n" + QLocale().formattedDataSize(preview.bytes, 1, QLocale::DataSizeSIFormat) + (image.isNull() ? "\nBundle icon unavailable" : ""));
    }
}
QStringList AppBrowser::selectedBundles() const {
    QStringList names; for (auto *item : selectedItems()) names << item->data(Qt::UserRole).toString(); return names;
}
QMimeData *AppBrowser::mimeData(const QList<QListWidgetItem *> &items) const {
    QJsonArray paths; for (auto *item : items) paths.append(item->data(Qt::UserRole).toString());
    auto *mime = new QMimeData; mime->setData(bundleMime, QJsonDocument(paths).toJson(QJsonDocument::Compact)); return mime;
}
QStringList AppBrowser::mimeTypes() const { return {bundleMime}; }
ImportTable::ImportTable(QWidget *parent) : QTableWidget(0, 3, parent) {
    setAcceptDrops(true); setDragDropMode(QAbstractItemView::DropOnly);
}
void ImportTable::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasFormat(bundleMime)) event->acceptProposedAction();
}
void ImportTable::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasFormat(bundleMime)) event->acceptProposedAction();
}
void ImportTable::dropEvent(QDropEvent *event) {
    if (!event->mimeData()->hasFormat(bundleMime)) return;
    auto document = QJsonDocument::fromJson(event->mimeData()->data(bundleMime));
    if (!document.isArray()) return;
    QStringList names;
    for (const auto &value : document.array()) { if (!value.isString()) return; names << value.toString(); }
    if (!names.isEmpty()) { event->acceptProposedAction(); emit bundlesDropped(names); }
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "appbrowser.h"
#include "log.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMimeData>
#include <QUrl>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QStyle>
#include <QPainter>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QApplication>
#include <QRubberBand>

namespace {
const QString bundleMime = "application/x-darling-app-bundles";
enum Roles { NameRole = Qt::UserRole + 1, SizeRole, StatusRole, StateRole, ActivityRole, ProgressRole };
class AppItem : public QListWidgetItem {
public:
    using QListWidgetItem::QListWidgetItem;
    bool operator<(const QListWidgetItem &other) const override {
        if (listWidget()->property("sortBySize").toBool() && data(SizeRole).toULongLong() != other.data(SizeRole).toULongLong()) return data(SizeRole).toULongLong() < other.data(SizeRole).toULongLong();
        return QString::localeAwareCompare(data(NameRole).toString(), other.data(NameRole).toString()) < 0;
    }
};
class AppDelegate : public QStyledItemDelegate {
public:
    explicit AppDelegate(AppBrowser *browser) : QStyledItemDelegate(browser), browser(browser) {}
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (browser->viewMode() == QListView::ListMode) option->text = index.data(NameRole).toString();
        if (browser->viewMode() == QListView::ListMode && !index.data(StatusRole).toString().isEmpty()) option->text += "    · " + index.data(StatusRole).toString();
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const QString status = index.data(StatusRole).toString();
        const bool gridStatus = browser->viewMode() == QListView::IconMode && !status.isEmpty();
        QStyleOptionViewItem content(option);
        const int lineHeight = option.fontMetrics.height();
        const bool listMode = browser->viewMode() == QListView::ListMode;
        if (gridStatus) content.rect.adjust(0, 0, 0, -lineHeight - 2);
        if (listMode) content.rect.adjust(0, 0, -SortHeader::SizeColumnWidth, 0);
        QStyledItemDelegate::paint(painter, content, index);
        if (listMode) {
            painter->save(); painter->setPen(option.palette.color(option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text));
            painter->drawText(QRect(option.rect.right() - SortHeader::SizeColumnWidth, option.rect.top(), SortHeader::SizeColumnWidth - 8, option.rect.height()), Qt::AlignRight | Qt::AlignVCenter, QLocale().formattedDataSize(index.data(SizeRole).toLongLong(), 1, QLocale::DataSizeSIFormat));
            painter->restore();
        }
        if (gridStatus) {
            painter->save(); painter->setPen(option.palette.color(option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text));
            painter->drawText(QRect(option.rect.left(), option.rect.bottom() - lineHeight, option.rect.width(), lineHeight), Qt::AlignHCenter | Qt::AlignVCenter, option.fontMetrics.elidedText(status, Qt::ElideRight, option.rect.width() - 4)); painter->restore();
        }
        const auto state = AppBrowser::State(index.data(ActivityRole).toInt());
        if (state != AppBrowser::State::Importing && state != AppBrowser::State::Launching && state != AppBrowser::State::Running && state != AppBrowser::State::Failed) return;
        const QColor color = state == AppBrowser::State::Failed ? QColor(200, 65, 65) : state == AppBrowser::State::Running ? QColor(45, 160, 85) : QColor(65, 125, 215);
        painter->save(); painter->setPen(Qt::NoPen); painter->setBrush(color);
        painter->drawEllipse(QRect(option.rect.right() - 14, option.rect.top() + 4, 8, 8));
        if (state == AppBrowser::State::Importing) {
            const int percent = index.data(ProgressRole).toInt();
            if (percent >= 0) painter->drawRect(QRect(option.rect.left() + 3, option.rect.bottom() - 3, (option.rect.width() - 6) * percent / 100, 3));
        }
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        if (browser->viewMode() == QListView::IconMode && !index.data(StatusRole).toString().isEmpty()) size.rheight() += option.fontMetrics.height() + 2;
        return size;
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
QList<QRect> AppBrowser::contentRects(const QModelIndex &index) const {
    const QRect cell = visualRect(index);
    const QFontMetrics metrics = fontMetrics();
    if (viewMode() == QListView::ListMode) {
        const QString text = index.data(Qt::DisplayRole).toString() + "    · " + index.data(StatusRole).toString();
        return {QRect(cell.left(), cell.top(), 12 + iconSize().width() + metrics.horizontalAdvance(text), cell.height()), QRect(cell.right() - SortHeader::SizeColumnWidth, cell.top(), SortHeader::SizeColumnWidth, cell.height())};
    }
    const int textWidth = qMin(cell.width(), metrics.horizontalAdvance(index.data(Qt::DisplayRole).toString()) + 8);
    const QRect icon(cell.center().x() - iconSize().width() / 2, cell.top(), iconSize().width(), iconSize().height() + 8);
    const QRect text(cell.center().x() - textWidth / 2, icon.bottom(), textWidth, cell.bottom() - icon.bottom());
    return {icon, text};
}
QRect AppBrowser::contentRect(const QModelIndex &index) const {
    if (viewMode() == QListView::ListMode) return contentRects(index).first();
    QRect all; for (const QRect &rect : contentRects(index)) all = all.united(rect);
    return all;
}
bool AppBrowser::onContent(const QModelIndex &index, const QPoint &point) const {
    for (const QRect &rect : contentRects(index)) if (rect.contains(point)) return true;
    return false;
}
void AppBrowser::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
        const QModelIndex index = indexAt(event->pos());
        if (index.isValid() && onContent(index, event->pos())) {
            if (!selectionModel()->isSelected(index)) selectionModel()->select(index, QItemSelectionModel::ClearAndSelect);
            selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
            dragCandidate = true; pressPosition = event->pos(); pressedItem = index;
            event->accept(); return;
        }
        clearSelection(); setCurrentIndex(QModelIndex());
        rubberOrigin = event->pos();
        if (!rubberBand) rubberBand = new QRubberBand(QRubberBand::Rectangle, viewport());
        rubberBand->setGeometry(QRect(rubberOrigin, QSize())); rubberBand->show();
        event->accept(); return;
    }
    QListWidget::mousePressEvent(event);
}
void AppBrowser::mouseMoveEvent(QMouseEvent *event) {
    if (dragCandidate) {
        if (!(event->buttons() & Qt::LeftButton)) { dragCandidate = false; return; }
        if ((event->pos() - pressPosition).manhattanLength() >= QApplication::startDragDistance()) { dragCandidate = false; startDrag(Qt::CopyAction); }
        event->accept(); return;
    }
    if (rubberBand && rubberBand->isVisible()) {
        const QRect area = QRect(rubberOrigin, event->pos()).normalized().intersected(viewport()->rect());
        rubberBand->setGeometry(area);
        QItemSelection hit;
        for (int row = 0; row < count(); ++row) {
            const QModelIndex index = model()->index(row, 0);
            if (isRowHidden(row)) continue;
            for (const QRect &rect : contentRects(index)) if (rect.intersects(area)) { hit.select(index, index); break; }
        }
        selectionModel()->select(hit, QItemSelectionModel::ClearAndSelect);
        event->accept(); return;
    }
    QListWidget::mouseMoveEvent(event);
}
void AppBrowser::mouseDoubleClickEvent(QMouseEvent *event) {
    const QModelIndex index = indexAt(event->pos());
    if (event->button() == Qt::LeftButton && index.isValid() && onContent(index, event->pos())) { emit itemDoubleClicked(item(index.row())); event->accept(); return; }
    mousePressEvent(event);
}
void AppBrowser::mouseReleaseEvent(QMouseEvent *event) {
    if (dragCandidate) {
        dragCandidate = false;
        if (pressedItem.isValid()) selectionModel()->select(pressedItem, QItemSelectionModel::ClearAndSelect);
        event->accept(); return;
    }
    if (rubberBand && rubberBand->isVisible()) { rubberBand->hide(); event->accept(); return; }
    QListWidget::mouseReleaseEvent(event);
}
void AppBrowser::setSorting(bool bySize, bool descending) {
    setProperty("sortingConfigured", true); setProperty("sortBySize", bySize); setProperty("sortDescending", descending);
    sortItems(descending ? Qt::DescendingOrder : Qt::AscendingOrder);
}
void AppBrowser::setGridView(bool grid) {
    setViewMode(grid ? QListView::IconMode : QListView::ListMode);
    setIconSize(grid ? QSize(64, 64) : QSize(32, 32));
    setGridSize(grid ? QSize(128, 128) : QSize());
    setWordWrap(grid); setSpacing(grid ? 8 : 2); setWrapping(grid);
    setMovement(QListView::Static); setResizeMode(QListView::Adjust);
    setDragEnabled(true); setAcceptDrops(dropsEnabled); viewport()->setAcceptDrops(dropsEnabled);
    viewport()->update();
}
void AppBrowser::showPreviews(const QList<AppPreview> &previews) {
    clear();
    for (const auto &preview : previews) {
        QImageReader reader(preview.iconPath);
        QImage image;
        if (!preview.iconPath.isEmpty()) image = reader.read();
        QIcon icon = image.isNull() ? style()->standardIcon(QStyle::SP_FileIcon) : QIcon(QPixmap::fromImage(image));
        auto *item = new AppItem(icon, preview.name, this);
        item->setData(Qt::UserRole, preview.relativeBundle); item->setData(NameRole, preview.name); item->setData(SizeRole, QVariant::fromValue(preview.bytes));
        item->setToolTip(preview.relativeBundle + "\n" + QLocale().formattedDataSize(preview.bytes, 1, QLocale::DataSizeSIFormat) + (image.isNull() ? "\nBundle icon unavailable" : ""));
    }
    if (property("sortingConfigured").toBool()) sortItems(property("sortDescending").toBool() ? Qt::DescendingOrder : Qt::AscendingOrder);
    applyFilter();
}
void AppBrowser::setSearch(const QString &query) { search = query; applyFilter(); }
void AppBrowser::setFilter(Filter value) { filter = value; applyFilter(); }
void AppBrowser::setImportedBundles(const QStringList &bundles) { importedBundles = bundles; applyFilter(); }
void AppBrowser::setAppState(const QString &bundle, Filter state) {
    for (int i = 0; i < count(); ++i) if (item(i)->data(Qt::UserRole).toString() == bundle) item(i)->setData(StateRole, int(state));
    applyFilter();
}
void AppBrowser::setActivity(const QString &bundle, State state, int percent) {
    static const QStringList labels{"Ready", "Importing…", "Launching…", "Running", "Failed", "Exited", "Queued"};
    for (int i = 0; i < count(); ++i) if (item(i)->data(Qt::UserRole).toString() == bundle) {
        item(i)->setData(ActivityRole, int(state)); item(i)->setData(ProgressRole, percent);
        item(i)->setData(StatusRole, labels.at(int(state)) + (percent >= 0 ? " " + QString::number(percent) + "%" : QString()));
    }
    setAppState(bundle, state == State::Failed ? Filter::Failed : state == State::Running || state == State::Launching ? Filter::Running : Filter::All);
    viewport()->update();
}
void AppBrowser::applyFilter() {
    int visible = 0;
    for (int i = 0; i < count(); ++i) {
        auto *entry = item(i);
        const bool match = entry->data(NameRole).toString().contains(search.trimmed(), Qt::CaseInsensitive);
        const bool imported = importedBundles.contains(entry->data(Qt::UserRole).toString());
        const bool state = filter == Filter::All || (filter == Filter::Imported ? imported : filter == Filter::NotImported ? !imported : entry->data(StateRole).toInt() == int(filter));
        entry->setHidden(!match || !state);
        if (entry->isHidden()) entry->setSelected(false); else ++visible;
    }
    emit visibleAppsChanged(visible);
}
QStringList AppBrowser::selectedBundles() const {
    QStringList names; for (auto *item : selectedItems()) if (!item->isHidden()) names << item->data(Qt::UserRole).toString(); return names;
}
QMimeData *AppBrowser::mimeData(const QList<QListWidgetItem *> &items) const {
    QJsonArray paths; for (auto *item : items) paths.append(item->data(Qt::UserRole).toString());
    auto *mime = new QMimeData; mime->setData(dragMimeType(), QJsonDocument(paths).toJson(QJsonDocument::Compact)); return mime;
}
QString AppBrowser::dragMimeType() const { return bundleMime; }
QStringList AppBrowser::mimeTypes() const { return {dragMimeType()}; }
ImportedBrowser::ImportedBrowser(QWidget *parent) : AppBrowser(parent) {
    dropsEnabled = true; setAcceptDrops(true); setDragDropMode(QAbstractItemView::DragDrop); viewport()->setAcceptDrops(true);
}
static bool acceptsDrop(const QMimeData *mime) { return mime->hasFormat(bundleMime) || mime->hasUrls(); }
void ImportedBrowser::dragEnterEvent(QDragEnterEvent *event) {
    LauncherLog::write("dnd", "drag enter formats=" + event->mimeData()->formats().join(",") + " proposed=" + QString::number(int(event->proposedAction())) + " possible=" + QString::number(int(event->possibleActions())) + " accepted=" + (acceptsDrop(event->mimeData()) ? "yes" : "no"));
    if (acceptsDrop(event->mimeData())) event->acceptProposedAction();
}
void ImportedBrowser::dragMoveEvent(QDragMoveEvent *event) {
    if (acceptsDrop(event->mimeData())) event->acceptProposedAction();
}
void ImportedBrowser::dropEvent(QDropEvent *event) {
    LauncherLog::write("dnd", "drop formats=" + event->mimeData()->formats().join(",") + " urls=" + QString::number(event->mimeData()->urls().size()));
    if (!event->mimeData()->hasFormat(bundleMime) && event->mimeData()->hasUrls()) {
        QStringList paths;
        for (const QUrl &url : event->mimeData()->urls()) if (url.isLocalFile()) paths << url.toLocalFile();
        if (!paths.isEmpty()) { event->acceptProposedAction(); emit pathsDropped(paths); }
        return;
    }
    if (!event->mimeData()->hasFormat(bundleMime)) return;
    auto document = QJsonDocument::fromJson(event->mimeData()->data(bundleMime));
    if (!document.isArray()) return;
    QStringList names;
    for (const auto &value : document.array()) { if (!value.isString()) return; names << value.toString(); }
    if (!names.isEmpty()) { event->acceptProposedAction(); emit bundlesDropped(names); }
}

QString ImportedBrowser::dragMimeType() const { return "application/x-darling-imported-bundles"; }
void ImportedBrowser::setStatus(const QString &bundle, const QString &status) {
    for (int i = 0; i < count(); ++i) if (item(i)->data(Qt::UserRole).toString() == bundle) {
        item(i)->setData(StatusRole, status); item(i)->setToolTip(bundle + "\n" + status); viewport()->update(); break;
    }
}
void ImportedBrowser::addPending(const QString &bundle, const QString &name, const QIcon &icon) {
    for (int i = 0; i < count(); ++i) if (item(i)->data(Qt::UserRole).toString() == bundle) return;
    auto *entry = new AppItem(icon, name, this);
    entry->setData(Qt::UserRole, bundle); entry->setData(NameRole, name); entry->setData(StatusRole, "Waiting for Darling");
    entry->setFlags(Qt::NoItemFlags); entry->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
    entry->setToolTip(name + "\nImports automatically when Darling is ready");
    applyFilter();
}
QString ImportedBrowser::status(const QString &bundle) const {
    for (int i = 0; i < count(); ++i) if (item(i)->data(Qt::UserRole).toString() == bundle) return item(i)->data(StatusRole).toString();
    return {};
}
TrashTarget::TrashTarget(QWidget *parent) : QLabel(parent) {
    setAcceptDrops(true); setPixmap(style()->standardIcon(QStyle::SP_TrashIcon).pixmap(36, 36));
    setToolTip("Drag imported apps here to move them to this prefix's trash"); setMinimumSize(48, 48); setAlignment(Qt::AlignCenter);
}
void TrashTarget::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasFormat("application/x-darling-imported-bundles")) event->acceptProposedAction();
}
void TrashTarget::dropEvent(QDropEvent *event) {
    const auto document = QJsonDocument::fromJson(event->mimeData()->data("application/x-darling-imported-bundles"));
    if (!document.isArray()) return;
    QStringList bundles; for (const auto &value : document.array()) { if (!value.isString()) return; bundles << value.toString(); }
    if (!bundles.isEmpty()) { event->acceptProposedAction(); emit bundlesDropped(bundles); }
}

SortHeader::SortHeader(QWidget *parent) : QWidget(parent) {
    auto *layout = new QHBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    nameButton = new QPushButton; nameButton->setObjectName("sortByName"); nameButton->setFlat(true); nameButton->setStyleSheet("text-align: left; font-weight: bold; padding-left: 40px;");
    sizeButton = new QPushButton; sizeButton->setObjectName("sortBySize"); sizeButton->setFlat(true); sizeButton->setFixedWidth(SizeColumnWidth); sizeButton->setStyleSheet("text-align: right; font-weight: bold; padding-right: 8px;");
    layout->addWidget(nameButton, 1); layout->addWidget(sizeButton);
    connect(nameButton, &QPushButton::clicked, this, [this] { setSort(false, !sizeColumn && !reverse); });
    connect(sizeButton, &QPushButton::clicked, this, [this] { setSort(true, sizeColumn && !reverse); });
    refreshLabels();
}
void SortHeader::setSort(bool bySize, bool descending) {
    sizeColumn = bySize; reverse = descending; refreshLabels(); emit sortChanged(sizeColumn, reverse);
}
void SortHeader::refreshLabels() {
    const QString arrow = reverse ? QStringLiteral(" ▼") : QStringLiteral(" ▲");
    nameButton->setText("Name" + (sizeColumn ? QString() : arrow)); sizeButton->setText("Size" + (sizeColumn ? arrow : QString()));
}

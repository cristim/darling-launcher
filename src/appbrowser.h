#pragma once
#include "core.h"
#include <QListWidget>
#include <QTableWidget>

class AppBrowser : public QListWidget {
    Q_OBJECT
public:
    explicit AppBrowser(QWidget *parent = nullptr);
    void showPreviews(const QList<AppPreview> &previews);
    void setGridView(bool grid);
    QStringList selectedBundles() const;
protected:
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override;
    QStringList mimeTypes() const override;
};

class ImportTable : public QTableWidget {
    Q_OBJECT
public:
    explicit ImportTable(QWidget *parent = nullptr);
signals:
    void bundlesDropped(const QStringList &bundles);
protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
};

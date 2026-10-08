// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
#include <QListWidget>
#include <QLabel>
#include <QPoint>
#include <QRubberBand>

class AppBrowser : public QListWidget {
    Q_OBJECT
public:
    explicit AppBrowser(QWidget *parent = nullptr);
    void showPreviews(const QList<AppPreview> &previews);
    void setGridView(bool grid);
    void setSorting(bool bySize, bool descending);
    QStringList selectedBundles() const;
    QRect contentRect(const QModelIndex &index) const;
    QList<QRect> contentRects(const QModelIndex &index) const;
    bool onContent(const QModelIndex &index, const QPoint &point) const;
    enum class Filter { All, Imported, Running, Failed, NotImported };
    void setSearch(const QString &query);
    void setFilter(Filter filter);
    void setImportedBundles(const QStringList &bundles);
    void setAppState(const QString &bundle, Filter state);
    enum class State { Ready, Importing, Launching, Running, Failed, Exited, Queued };
    void setActivity(const QString &bundle, State state, int percent = -1);
signals:
    void visibleAppsChanged(int count);
private:
    QString search;
    Filter filter = Filter::All;
    QStringList importedBundles;
    void applyFilter();
    bool dragCandidate = false;
    QPoint pressPosition;
    QPersistentModelIndex pressedItem;
    QRubberBand *rubberBand = nullptr;
    QPoint rubberOrigin;
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override;
    QStringList mimeTypes() const override;
    virtual QString dragMimeType() const;
};

class ImportedBrowser : public AppBrowser {
    Q_OBJECT
public:
    explicit ImportedBrowser(QWidget *parent = nullptr);
    void setStatus(const QString &bundle, const QString &status);
    QString status(const QString &bundle) const;
signals:
    void bundlesDropped(const QStringList &bundles);
protected:
    QString dragMimeType() const override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
};

class TrashTarget : public QLabel {
    Q_OBJECT
public:
    explicit TrashTarget(QWidget *parent = nullptr);
signals:
    void bundlesDropped(const QStringList &bundles);
protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
};

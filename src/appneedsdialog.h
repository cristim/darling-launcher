// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "appneeds.h"
#include "appneedscache.h"
#include "automount.h"
#include "mount.h"
#include <QDialog>
#include <QList>

// What the background analysis of an imported app found; `error` is set when the names could not be read.
struct AppNeedsAnalysis {
    QString appName;
    AppNeeds::Plan plan;
    AppNeedsCache::Request request;
    AppNeedsCache::Preview cache;
    QString error;
};

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

// Step 1 after an app import: what the app links that has resource files to copy. Rows the family rule selected start
// checked, optional rows start unchecked; skipped frameworks are listed with their reasons. Nothing is copied here:
// accepting only hands the user's choice (plan()) back to the caller, which then asks nothing further.
class AppNeedsDialog : public QDialog {
    Q_OBJECT
public:
    explicit AppNeedsDialog(const AppNeeds::Plan &plan, QWidget *parent = nullptr);
    AppNeeds::Plan plan() const;  // the plan with the rows' current choices
private:
    AppNeeds::Plan current;
    QTableWidget *table = nullptr;
    QLabel *totals = nullptr;
    QPushButton *importButton = nullptr;
    QList<int> rowItem;           // table row -> index in current.items
    void update();
};

// Step 2, only for a cache app: the preview of the dyld cache copy and the Apple-binaries notice. The copy button stays
// disabled until the user ticks that they understand the notice, and while the step cannot be offered.
class CacheOfferDialog : public QDialog {
    Q_OBJECT
public:
    CacheOfferDialog(const AppNeedsCache::Preview &preview, QWidget *parent = nullptr);
signals:
    void mountRequested();        // the cache volume is not mounted: run the explained mount flow for it
};

// Before any authorization prompt: why root is needed, what will be mounted and what will not happen. Continue starts
// the helpers (the caller does that); Cancel stops. With several APFS partitions the user picks one, with sizes.
class MountExplainDialog : public QDialog {
    Q_OBJECT
public:
    MountExplainDialog(const AutoMount::Need &need, const QList<MacPartition> &partitions, QWidget *parent = nullptr);
    MacPartition chosen() const;
private:
    QList<MacPartition> partitions;
    QComboBox *choice = nullptr;
};

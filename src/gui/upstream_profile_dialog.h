#pragma once

#include "core/upstream_profile.h"

#include <QtWidgets/QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class UpstreamProfileDialog : public QDialog {
    Q_OBJECT

public:
    explicit UpstreamProfileDialog(net_tunnel::UpstreamProfileStore *store,
                                   const QString &language,
                                   const QString &activeProfileId = QString(),
                                   bool proxyRunning = false,
                                   QWidget *parent = 0);

    QString selectedProfileId() const;

public slots:
    void setLanguage(const QString &language);
    void setRuntimeState(bool proxyRunning, const QString &activeProfileId);
    void refresh();

signals:
    void selectedProfileChanged(const QString &profileId);

private slots:
    void addProfile();
    void viewOrEditSelectedProfile();
    void removeSelectedProfile();
    void selectCurrentProfile();
    void importProfiles();
    void exportProfiles();
    void applySearch();
    void changePageSize(int index);
    void changeSort(int logicalIndex);
    void firstPage();
    void previousPage();
    void nextPage();
    void lastPage();
    void updateActionStates();

private:
    void buildUi();
    void retranslateUi();
    void loadPage(const QString &preferredRowId = QString());
    void loadPageContaining(const QString &profileId);
    QString textFor(const QString &key) const;
    QString currentRowProfileId() const;
    bool currentRowProfile(net_tunnel::UpstreamProfile *profile) const;
    bool profileIsLocked(const QString &profileId) const;
    void showStoreError(const QString &operation, const QString &error);

    net_tunnel::UpstreamProfileStore *store_;
    QString language_;
    QString activeProfileId_;
    bool proxyRunning_;
    int currentPage_;
    int pageSize_;
    int totalPages_;
    net_tunnel::UpstreamProfileSortField sortField_;
    Qt::SortOrder sortOrder_;
    QString selectedProfileId_;

    QLineEdit *searchEdit_;
    QTableWidget *table_;
    QPushButton *addButton_;
    QPushButton *viewEditButton_;
    QPushButton *removeButton_;
    QPushButton *selectButton_;
    QPushButton *importButton_;
    QPushButton *exportButton_;
    QPushButton *firstPageButton_;
    QPushButton *previousPageButton_;
    QPushButton *nextPageButton_;
    QPushButton *lastPageButton_;
    QPushButton *closeButton_;
    QComboBox *pageSizeCombo_;
    QLabel *pageLabel_;
    QLabel *summaryLabel_;
};

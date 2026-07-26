#include "core/upstream_profile.h"
#include "gui/upstream_profile_dialog.h"

#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QLockFile>
#include <QtGui/QClipboard>
#include <QtGui/QImage>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QToolButton>

using namespace net_tunnel;

class UpstreamProfileDialogTest : public QObject {
    Q_OBJECT

private slots:
    void paginatesAndSelectsCurrentProfile();
    void locksActiveProfileWhileProxyRuns();
    void editorUsesDefaultsAndProtectsApiKey();
    void editingNormalizesApiKeyWhitespace();
    void addsWithoutSelectingWhileSelectionIsLocked();
};

static UpstreamProfile addProfile(UpstreamProfileStore *store,
                                  const QString &name,
                                  const QString &apiKey = QString())
{
    UpstreamProfile profile;
    profile.displayName = name;
    QString hostLabel = name.toLower();
    hostLabel.replace(' ', '-');
    profile.baseUrl = QString("https://%1.example.com/v1").arg(hostLabel);
    profile.apiKey = apiKey;
    profile.userAgent = "curl/8.7.1";
    profile.forwardUserAgent = false;
    profile.upstreamTimeoutSec = 1800;
    profile.firstTokenTimeoutSec = 30;
    QString error;
    const bool added = store->addProfile(&profile, &error);
    if (!added) qWarning("addProfile failed: %s", qPrintable(error));
    return profile;
}

void UpstreamProfileDialogTest::paginatesAndSelectsCurrentProfile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    UpstreamProfileStore store(dir.filePath("profiles.sqlite3"));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));

    QList<UpstreamProfile> profiles;
    for (int i = 0; i < 25; ++i) {
        profiles.append(addProfile(&store, QString("Profile %1").arg(i, 2, 10, QLatin1Char('0'))));
        QVERIFY(!profiles.last().id.isEmpty());
    }
    QVERIFY2(store.setSelectedProfileId(profiles.first().id, &error), qPrintable(error));

    UpstreamProfileDialog dialog(&store, "en");
    QVERIFY(dialog.windowFlags() & Qt::FramelessWindowHint);
    QVERIFY(dialog.findChild<QWidget *>("guardDialogTitleBar"));
    QTableWidget *table = dialog.findChild<QTableWidget *>("profileTable");
    QComboBox *pageSize = dialog.findChild<QComboBox *>("profilePageSizeCombo");
    QLineEdit *search = dialog.findChild<QLineEdit *>("profileSearchEdit");
    QVERIFY(table);
    QVERIFY(pageSize);
    QVERIFY(search);
    QCOMPARE(pageSize->currentData().toInt(), 20);
    QCOMPARE(table->rowCount(), 20);

    pageSize->setCurrentIndex(0);
    QCOMPARE(pageSize->currentData().toInt(), 10);
    QCOMPARE(table->rowCount(), 10);
    pageSize->setCurrentIndex(2);
    QCOMPARE(pageSize->currentData().toInt(), 50);
    QCOMPARE(table->rowCount(), 25);
    pageSize->setCurrentIndex(3);
    QCOMPARE(pageSize->currentData().toInt(), 100);
    QCOMPARE(table->rowCount(), 25);
    pageSize->setCurrentIndex(1);
    QCOMPARE(table->rowCount(), 20);

    QVERIFY(QMetaObject::invokeMethod(&dialog, "nextPage", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 5);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "firstPage", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 20);

    search->setText("Profile 24");
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 0)->text(), QString("Profile 24"));
    search->clear();
    QVERIFY(QMetaObject::invokeMethod(&dialog, "changeSort", Qt::DirectConnection,
                                      Q_ARG(int, 0)));
    QCOMPARE(table->item(0, 0)->text(), QString("Profile 00"));

    QString rowId;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QString candidate = table->item(row, 0)->data(Qt::UserRole).toString();
        if (candidate != profiles.first().id) {
            table->selectRow(row);
            rowId = candidate;
            break;
        }
    }
    QVERIFY(!rowId.isEmpty());
    QSignalSpy spy(&dialog, SIGNAL(selectedProfileChanged(QString)));
    QPushButton *select = dialog.findChild<QPushButton *>("profileSelectButton");
    QVERIFY(select);
    QVERIFY(select->isEnabled());
    select->click();
    QCOMPARE(store.selectedProfileId(&error), rowId);
    QCOMPARE(spy.count(), 1);
}

void UpstreamProfileDialogTest::locksActiveProfileWhileProxyRuns()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    UpstreamProfileStore store(dir.filePath("profiles.sqlite3"));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    const UpstreamProfile active = addProfile(&store, "Active");
    const UpstreamProfile idle = addProfile(&store, "Idle");
    QVERIFY2(store.setSelectedProfileId(active.id, &error), qPrintable(error));

    UpstreamProfileDialog dialog(&store, "zh", active.id, true);
    QTableWidget *table = dialog.findChild<QTableWidget *>("profileTable");
    QVERIFY(table);
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->data(Qt::UserRole).toString() == active.id) {
            table->selectRow(row);
            break;
        }
    }
    QPushButton *viewEdit = dialog.findChild<QPushButton *>("profileViewEditButton");
    QPushButton *remove = dialog.findChild<QPushButton *>("profileRemoveButton");
    QPushButton *select = dialog.findChild<QPushButton *>("profileSelectButton");
    QVERIFY(viewEdit);
    QVERIFY(remove);
    QVERIFY(select);
    QCOMPARE(viewEdit->text(), QString::fromUtf8("查看"));
    QVERIFY(!remove->isEnabled());
    QVERIFY(!select->isEnabled());

    dialog.setRuntimeState(false, QString());
    QCOMPARE(viewEdit->text(), QString::fromUtf8("编辑"));
    QVERIFY(remove->isEnabled());

    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->data(Qt::UserRole).toString() == idle.id) {
            table->selectRow(row);
            break;
        }
    }
    QVERIFY(select->isEnabled());
}

void UpstreamProfileDialogTest::editorUsesDefaultsAndProtectsApiKey()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    UpstreamProfileStore store(dir.filePath("profiles.sqlite3"));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    const UpstreamProfile secret = addProfile(&store, "Secret", "sk-test-secret");
    QVERIFY(!secret.id.isEmpty());

    UpstreamProfileDialog dialog(&store, "en");
    QTableWidget *table = dialog.findChild<QTableWidget *>("profileTable");
    QVERIFY(table);
    table->selectRow(0);

    bool keyChecksPassed = false;
    QTimer::singleShot(0, [&keyChecksPassed]() {
        QDialog *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!editor) return;
        QLineEdit *key = editor->findChild<QLineEdit *>("profileApiKeyEdit");
        QToolButton *reveal = editor->findChild<QToolButton *>("profileRevealApiKeyButton");
        QToolButton *copy = editor->findChild<QToolButton *>("profileCopyApiKeyButton");
        if (key && reveal && copy && key->echoMode() == QLineEdit::Password &&
            (editor->windowFlags() & Qt::FramelessWindowHint) &&
            editor->findChild<QWidget *>("guardDialogTitleBar")) {
            const quint64 concealedIconKey = reveal->icon().cacheKey();
            const QImage concealedIcon = reveal->icon().pixmap(18, 18).toImage();
            reveal->click();
            copy->click();
            const QImage revealedIcon = reveal->icon().pixmap(18, 18).toImage();
            keyChecksPassed = key->echoMode() == QLineEdit::Normal &&
                QApplication::clipboard()->text() == "sk-test-secret" &&
                !reveal->icon().isNull() && reveal->icon().cacheKey() != concealedIconKey &&
                concealedIcon.pixelColor(3, 3).alpha() > 0 &&
                revealedIcon.pixelColor(3, 3).alpha() == 0;
        }
        editor->reject();
    });
    QVERIFY(QMetaObject::invokeMethod(&dialog, "viewOrEditSelectedProfile", Qt::DirectConnection));
    QVERIFY(keyChecksPassed);

    bool defaultsPassed = false;
    QTimer::singleShot(0, [&defaultsPassed]() {
        QDialog *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!editor) return;
        QLineEdit *userAgent = editor->findChild<QLineEdit *>("profileUserAgentEdit");
        QSpinBox *upstreamTimeout = editor->findChild<QSpinBox *>("profileUpstreamTimeoutSpin");
        QSpinBox *firstTokenTimeout = editor->findChild<QSpinBox *>("profileFirstTokenTimeoutSpin");
        defaultsPassed = userAgent && upstreamTimeout && firstTokenTimeout &&
            userAgent->text() == "curl/8.7.1" &&
            upstreamTimeout->value() == 1800 && firstTokenTimeout->value() == 30;
        editor->reject();
    });
    QVERIFY(QMetaObject::invokeMethod(&dialog, "addProfile", Qt::DirectConnection));
    QVERIFY(defaultsPassed);
}

void UpstreamProfileDialogTest::editingNormalizesApiKeyWhitespace()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    UpstreamProfileStore store(dir.filePath("profiles.sqlite3"));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    const QString paddedApiKey = "  secret-with-spaces  ";
    const QString normalizedApiKey = "secret-with-spaces";
    const UpstreamProfile profile = addProfile(&store, "Whitespace Secret", "original-key");
    QVERIFY(!profile.id.isEmpty());
    QCOMPARE(profile.apiKey, QString("original-key"));

    UpstreamProfileDialog dialog(&store, "en");
    QTableWidget *table = dialog.findChild<QTableWidget *>("profileTable");
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 1);
    table->selectRow(0);

    bool submitted = false;
    QTimer::singleShot(0, [&submitted, &paddedApiKey]() {
        QDialog *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!editor) return;
        QLineEdit *key = editor->findChild<QLineEdit *>("profileApiKeyEdit");
        QDialogButtonBox *buttons = editor->findChild<QDialogButtonBox *>("profileEditorButtons");
        if (!key || !buttons || key->text() != "original-key") {
            editor->reject();
            return;
        }
        key->setText(paddedApiKey);
        submitted = true;
        buttons->button(QDialogButtonBox::Save)->click();
    });
    QVERIFY(QMetaObject::invokeMethod(&dialog, "viewOrEditSelectedProfile", Qt::DirectConnection));
    QVERIFY(submitted);

    UpstreamProfile fetched;
    QVERIFY2(store.profileById(profile.id, &fetched, &error), qPrintable(error));
    QCOMPARE(fetched.apiKey, normalizedApiKey);
}

void UpstreamProfileDialogTest::addsWithoutSelectingWhileSelectionIsLocked()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath("profiles.sqlite3");
    UpstreamProfileStore store(databasePath);
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));

    QLockFile selectionLock(databasePath + ".selection.lock");
    selectionLock.setStaleLockTime(30000);
    QVERIFY(selectionLock.tryLock(0));

    UpstreamProfileDialog dialog(&store, "en");
    bool submitted = false;
    QTimer::singleShot(0, [&submitted]() {
        QDialog *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!editor) return;
        QLineEdit *name = editor->findChild<QLineEdit *>("profileNameEdit");
        QLineEdit *baseUrl = editor->findChild<QLineEdit *>("profileBaseUrlEdit");
        QDialogButtonBox *buttons = editor->findChild<QDialogButtonBox *>("profileEditorButtons");
        if (!name || !baseUrl || !buttons) {
            editor->reject();
            return;
        }
        name->setText("Added While Locked");
        baseUrl->setText("https://locked.example.com/v1");
        submitted = true;
        buttons->button(QDialogButtonBox::Save)->click();
    });
    QVERIFY(QMetaObject::invokeMethod(&dialog, "addProfile", Qt::DirectConnection));
    QVERIFY(submitted);

    UpstreamProfilePage page;
    QVERIFY2(store.listProfiles(QString(), 1, 20, SortByUpdatedAt, Qt::DescendingOrder,
                                &page, &error), qPrintable(error));
    QCOMPARE(page.totalItems, 1);
    QCOMPARE(store.selectedProfileId(&error), QString());
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(dialog.selectedProfileId(), QString());
}

QTEST_MAIN(UpstreamProfileDialogTest)
#include "upstream_profile_dialog_test.moc"

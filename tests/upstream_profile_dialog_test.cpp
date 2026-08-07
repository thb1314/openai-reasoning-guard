#include "core/upstream_profile.h"
#include "gui/upstream_profile_dialog.h"

#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QLockFile>
#include <QtGui/QClipboard>
#include <QtGui/QFont>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QScreen>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QWidget>

using namespace net_tunnel;

class UpstreamProfileDialogTest : public QObject {
    Q_OBJECT

private slots:
    void paginatesAndSelectsCurrentProfile();
    void keepsActiveProfileReadOnlyAndEditsIdleProfileWhileProxyRuns();
    void editorUsesDefaultsAndProtectsApiKey();
    void editingNormalizesApiKeyWhitespace();
    void addsWithoutSelectingWhileSelectionIsLocked();
    void inheritsParentUiScaleAcrossListAndEditor();
    void largeScaleDialogFitsScreenAndTableScrollsHorizontally();
};

static QRect safeDialogGeometry()
{
    return QGuiApplication::primaryScreen()->availableGeometry().adjusted(24, 24, -24, -24);
}

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

void UpstreamProfileDialogTest::keepsActiveProfileReadOnlyAndEditsIdleProfileWhileProxyRuns()
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

    bool activeReadOnly = false;
    QTimer::singleShot(0, [&activeReadOnly]() {
        QDialog *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!editor) return;
        QSpinBox *retryAfter =
            editor->findChild<QSpinBox *>("profileRetryAfterOverrideSpin");
        activeReadOnly = retryAfter && retryAfter->isReadOnly();
        editor->reject();
    });
    QVERIFY(QMetaObject::invokeMethod(&dialog, "viewOrEditSelectedProfile",
                                      Qt::DirectConnection));
    QVERIFY(activeReadOnly);

    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->data(Qt::UserRole).toString() == idle.id) {
            table->selectRow(row);
            break;
        }
    }
    QCOMPARE(viewEdit->text(), QString::fromUtf8("编辑"));
    QVERIFY(remove->isEnabled());
    QVERIFY(select->isEnabled());

    bool idleSubmitted = false;
    QTimer::singleShot(0, [&idleSubmitted]() {
        QDialog *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!editor) return;
        QSpinBox *retryAfter =
            editor->findChild<QSpinBox *>("profileRetryAfterOverrideSpin");
        QDialogButtonBox *buttons =
            editor->findChild<QDialogButtonBox *>("profileEditorButtons");
        if (!retryAfter || !buttons || retryAfter->isReadOnly()) {
            editor->reject();
            return;
        }
        retryAfter->setValue(45);
        idleSubmitted = true;
        buttons->button(QDialogButtonBox::Save)->click();
    });
    QVERIFY(QMetaObject::invokeMethod(&dialog, "viewOrEditSelectedProfile",
                                      Qt::DirectConnection));
    QVERIFY(idleSubmitted);
    UpstreamProfile updatedIdle;
    QVERIFY2(store.profileById(idle.id, &updatedIdle, &error), qPrintable(error));
    QCOMPARE(updatedIdle.retryAfterOverrideSec, QString("45"));

    QSignalSpy activation(&dialog, SIGNAL(profileActivationRequested(QString)));
    select->click();
    QCOMPARE(activation.count(), 1);
    QCOMPARE(activation.at(0).at(0).toString(), idle.id);
    QCOMPARE(store.selectedProfileId(&error), active.id);
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
        QSpinBox *retryAfterOverride = editor->findChild<QSpinBox *>("profileRetryAfterOverrideSpin");
        QCheckBox *mapErrors =
            editor->findChild<QCheckBox *>("profileMapUpstreamErrorsTo502Check");
        defaultsPassed = userAgent && upstreamTimeout && firstTokenTimeout &&
            retryAfterOverride && mapErrors &&
            userAgent->text() == "curl/8.7.1" &&
            upstreamTimeout->value() == 1800 && firstTokenTimeout->value() == 30 &&
            retryAfterOverride->value() == 0 && !mapErrors->isChecked();
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
        QSpinBox *retryAfterOverride = editor->findChild<QSpinBox *>("profileRetryAfterOverrideSpin");
        QCheckBox *mapErrors =
            editor->findChild<QCheckBox *>("profileMapUpstreamErrorsTo502Check");
        QDialogButtonBox *buttons = editor->findChild<QDialogButtonBox *>("profileEditorButtons");
        if (!key || !retryAfterOverride || !mapErrors || !buttons ||
            key->text() != "original-key") {
            editor->reject();
            return;
        }
        key->setText(paddedApiKey);
        retryAfterOverride->setValue(30);
        mapErrors->setChecked(true);
        submitted = true;
        buttons->button(QDialogButtonBox::Save)->click();
    });
    QVERIFY(QMetaObject::invokeMethod(&dialog, "viewOrEditSelectedProfile", Qt::DirectConnection));
    QVERIFY(submitted);

    UpstreamProfile fetched;
    QVERIFY2(store.profileById(profile.id, &fetched, &error), qPrintable(error));
    QCOMPARE(fetched.apiKey, normalizedApiKey);
    QCOMPARE(fetched.retryAfterOverrideSec, QString("30"));
    QCOMPARE(fetched.mapUpstreamErrorsTo502, true);
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

void UpstreamProfileDialogTest::inheritsParentUiScaleAcrossListAndEditor()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    UpstreamProfileStore store(dir.filePath("profiles.sqlite3"));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY(!addProfile(&store, "Scaled Profile", "sk-scale-test").id.isEmpty());

    QWidget parent;
    parent.setProperty("ui_scale_factor", 1.30);
    parent.setAttribute(Qt::WA_DontShowOnScreen);
    parent.show();

    UpstreamProfileDialog dialog(&store, "en", QString(), false, &parent);
    const QFont applicationFont = QApplication::font();
    const qreal applicationPointSize = applicationFont.pointSizeF();
    const QString dialogStyleSheet = dialog.styleSheet();
    QTableWidget *table = dialog.findChild<QTableWidget *>("profileTable");
    QComboBox *pageSize = dialog.findChild<QComboBox *>("profilePageSizeCombo");
    QPushButton *firstPage = dialog.findChild<QPushButton *>("profileFirstPageButton");
    QWidget *titleBar = dialog.findChild<QWidget *>("guardDialogTitleBar");
    QVERIFY(table);
    QVERIFY(pageSize);
    QVERIFY(firstPage);
    QVERIFY(titleBar);
    const QString tableStyleSheet = table->styleSheet();
    const qreal baseTablePointSize = table->font().pointSizeF();
    const int basePageSizeHeight = pageSize->sizeHint().height();

    dialog.setAttribute(Qt::WA_DontShowOnScreen);
    dialog.show();
    QTest::qWait(20);

    QVERIFY(qAbs(dialog.property("ui_scale_factor").toReal() - 1.30) < 0.001);
    const QRect safeGeometry = safeDialogGeometry();
    const QSize expectedMinimum = QSize(1144, 728).boundedTo(safeGeometry.size());
    const QSize expectedSize = QSize(1404, 884).boundedTo(safeGeometry.size())
        .expandedTo(expectedMinimum);
    QCOMPARE(dialog.minimumSize(), expectedMinimum);
    QCOMPARE(dialog.size(), expectedSize);
    QVERIFY(safeGeometry.contains(dialog.geometry()));
    QCOMPARE(titleBar->height(), 39);
    QCOMPARE(firstPage->size(), QSize(44, 39));
    QCOMPARE(table->verticalHeader()->defaultSectionSize(), 44);
    QCOMPARE(table->rowHeight(0), 44);
    QVERIFY(pageSize->minimumHeight() >= qRound(basePageSizeHeight * 1.30));
    QVERIFY(pageSize->styleSheet().contains("QComboBox::drop-down"));
    QVERIFY(pageSize->styleSheet().contains("width: 20px"));
    if (baseTablePointSize > 0.0) {
        QVERIFY(table->font().pointSizeF() > baseTablePointSize);
    }
    QCOMPARE(QApplication::font(), applicationFont);
    QCOMPARE(dialog.styleSheet(), dialogStyleSheet);
    QCOMPARE(table->styleSheet(), tableStyleSheet);

    table->selectRow(0);
    bool editorScaled = false;
    QTimer::singleShot(0, [&editorScaled, applicationPointSize, safeGeometry]() {
        QDialog *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!editor) return;
        QWidget *editorTitleBar = editor->findChild<QWidget *>("guardDialogTitleBar");
        QPushButton *editorClose = editor->findChild<QPushButton *>("guardDialogCloseButton");
        QFormLayout *form = editor->findChild<QFormLayout *>();
        QLineEdit *name = editor->findChild<QLineEdit *>("profileNameEdit");
        QSpinBox *upstreamTimeout =
            editor->findChild<QSpinBox *>("profileUpstreamTimeoutSpin");
        QCheckBox *forwardUserAgent =
            editor->findChild<QCheckBox *>("profileForwardUserAgentCheck");
        editorScaled = qAbs(editor->property("ui_scale_factor").toReal() - 1.30) < 0.001 &&
            editorTitleBar && editorTitleBar->height() == 39 &&
            editorClose && editorClose->size() == QSize(39, 39) &&
            editor->minimumWidth() == qMin(806, safeGeometry.width()) &&
            safeGeometry.contains(editor->geometry()) &&
            form && form->horizontalSpacing() == 21 &&
            form->verticalSpacing() == 13 && name && upstreamTimeout &&
            upstreamTimeout->styleSheet().contains("width: 13px") &&
            forwardUserAgent && forwardUserAgent->styleSheet().contains("width: 17px") &&
            (applicationPointSize <= 0.0 ||
             qAbs(name->font().pointSizeF() - applicationPointSize * 1.30) < 0.05);
        editor->reject();
    });
    QVERIFY(QMetaObject::invokeMethod(&dialog, "viewOrEditSelectedProfile", Qt::DirectConnection));
    QVERIFY(editorScaled);

    dialog.hide();
    parent.hide();
}

void UpstreamProfileDialogTest::largeScaleDialogFitsScreenAndTableScrollsHorizontally()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    UpstreamProfileStore store(dir.filePath("profiles.sqlite3"));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY(!addProfile(&store, "Large Scale Profile", "sk-large-scale").id.isEmpty());

    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    QWidget parent;
    const qreal applicationPointSize = QApplication::font().pointSizeF();
    const qreal scale = applicationPointSize > 0.0 ? 20.0 / applicationPointSize : 2.0;
    parent.setProperty("ui_scale_factor", scale);
    parent.setGeometry(available);
    parent.setAttribute(Qt::WA_DontShowOnScreen);
    parent.showMaximized();

    UpstreamProfileDialog dialog(&store, "en", QString(), false, &parent);
    QTableWidget *table = dialog.findChild<QTableWidget *>("profileTable");
    QVERIFY(table);
    QScrollBar *horizontal = table->horizontalScrollBar();
    QVERIFY(horizontal);
    const int baseScrollBarHeight = horizontal->sizeHint().height();

    dialog.setAttribute(Qt::WA_DontShowOnScreen);
    dialog.show();
    QTest::qWait(20);

    const QRect safeGeometry = safeDialogGeometry();
    QVERIFY(safeGeometry.contains(dialog.geometry()));
    QVERIFY(dialog.minimumWidth() <= safeGeometry.width());
    QVERIFY(dialog.minimumHeight() <= safeGeometry.height());
    QCOMPARE(table->horizontalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    QCOMPARE(table->horizontalScrollMode(), QAbstractItemView::ScrollPerPixel);
    QCOMPARE(table->horizontalHeader()->sectionResizeMode(0), QHeaderView::Interactive);
    QVERIFY(horizontal->isVisible());
    QVERIFY(horizontal->maximum() > horizontal->minimum());
    QVERIFY(horizontal->height() >= qRound(baseScrollBarHeight * scale));

    const int initialItemX = table->visualItemRect(table->item(0, 0)).x();
    horizontal->setValue(horizontal->maximum());
    QCoreApplication::processEvents();
    QCOMPARE(horizontal->value(), horizontal->maximum());
    QVERIFY(table->visualItemRect(table->item(0, 0)).x() < initialItemX);

    dialog.hide();
    parent.hide();
}

QTEST_MAIN(UpstreamProfileDialogTest)
#include "upstream_profile_dialog_test.moc"

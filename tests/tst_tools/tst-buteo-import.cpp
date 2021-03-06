/*
 * Copyright 2015 Canonical Ltd.
 *
 * This file is part of address-book-service.
 *
 * sync-monitor is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * contact-service-app is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QtCore>
#include <QtDBus>
#include <QTest>
#include <QtContacts>
#include <QSignalSpy>

#include "ab-update.h"
#include "config.h"

#define ADDRESS_BOOK_BUS_NAME   "com.canonical.pim"
#define ADDRESS_BOOK_OBJ        "/com/canonical/pim/AddressBook"
#define ADDRESS_BOOK_IFACE      "com.canonical.pim.AddressBook"

using namespace QtContacts;

class TstButeoImport : public QObject
{
    Q_OBJECT

    QTemporaryFile *m_settingsFile;

private:
    void createSource(const QString &sourceId,
                      const QString &sourceName,
                      const QString &provider,
                      const QString &applicationId,
                      quint32 accountId,
                      bool readOnly,
                      bool primary)
    {
        QDBusMessage createSource = QDBusMessage::createMethodCall(ADDRESS_BOOK_BUS_NAME,
                                                                   ADDRESS_BOOK_OBJ,
                                                                   ADDRESS_BOOK_IFACE,
                                                                   "createSource");
        QList<QVariant> args;
        args << sourceId
             << sourceName
             << provider
             << applicationId
             << accountId
             << readOnly
             << primary;
        createSource.setArguments(args);
        QDBusReply<bool> reply = QDBusConnection::sessionBus().call(createSource);
        if (reply.error().isValid()) {
            qWarning() << "Fail to create source" << reply.error();
        } else {
            qDebug() << "Source created" << reply.value();
        }
        QVERIFY(reply.value());
    }

    void resetAddressBook()
    {
        QDBusMessage reset = QDBusMessage::createMethodCall(ADDRESS_BOOK_BUS_NAME,
                                                            ADDRESS_BOOK_OBJ,
                                                            ADDRESS_BOOK_IFACE,
                                                           "reset");
        QDBusReply<bool> reply = QDBusConnection::sessionBus().call(reset);
        QVERIFY(reply.value());
    }

private Q_SLOTS:
    void init()
    {
        // we need `galera` manager to run this test
        QCoreApplication::setLibraryPaths(QStringList() << QT_PLUGINS_BINARY_DIR);
        QVERIFY(QContactManager::availableManagers().contains("galera"));

        m_settingsFile = new QTemporaryFile;
        QVERIFY(m_settingsFile->open());
        qDebug() << "Using as temporary file:" << m_settingsFile->fileName();
        //Make sure that we use a new settings every time that we run the test
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, m_settingsFile->fileName());
    }

    void cleanup()
    {
        resetAddressBook();
        delete m_settingsFile;
    }

    void tst_isOutDated()
    {
        ABUpdate updater;
        updater.skipNetworkTest();
        updater.setSilenceMode(true);

        QCOMPARE(updater.needsUpdate().count(), 1);
        QVERIFY(!updater.isRunning());
    }

    void tst_importAccount()
    {
        // populate sources
        createSource("source@1", "renato.teste2@gmail.com", "google", "", 0, false, true);
        createSource("source@2", "renato.teste3@gmail.com", "google", "", 0, false, false);
        createSource("source@3", "source-3", "google", "", 0, false, false);
        createSource("source@4", "source-4", "google", "", 0, false, false);

        ABUpdate updater;
        updater.skipNetworkTest();
        updater.setSilenceMode(true);

        QSignalSpy updatedSignal(&updater, SIGNAL(updateDone()));
        QSignalSpy updateErrorSignal(&updater, SIGNAL(updateError(QString, ABUpdateModule::ImportError)));

        updater.startUpdate();
        QVERIFY(updater.isRunning());
        QTRY_COMPARE(updatedSignal.count(), 1);
        QTRY_COMPARE(updateErrorSignal.count(), 0);
        QCOMPARE(updater.isRunning(), false);

        // Check if old sources was deleted
        QContactManager manager("galera");
        QContactDetailFilter sourceFilter;
        sourceFilter.setDetailType(QContactDetail::TypeType, QContactType::FieldType);
        sourceFilter.setValue(QContactType::TypeGroup);
        QCOMPARE(manager.contacts(sourceFilter).size(), 2);
    }
};

QTEST_MAIN(TstButeoImport)

#include "tst-buteo-import.moc"

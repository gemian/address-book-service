/*
 * Copyright 2013 Canonical Ltd.
 *
 * This file is part of contact-service-app.
 *
 * contact-service-app is free software; you can redistribute it and/or modify
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

#include <QObject>
#include <QtTest>
#include <QDebug>
#include <QtContacts>

#include "config.h"

using namespace QtContacts;

class AddressBookTest : public QObject
{
    Q_OBJECT
private:
    QContactManager *m_manager;

    QContact testContact()
    {
        // create a contact
        QContact contact;

        QContactName name;
        name.setFirstName("Fulano");
        name.setMiddleName("de");
        name.setLastName("Tal");
        contact.saveDetail(&name);

        QContactEmailAddress email;
        email.setEmailAddress("fulano@email.com");
        contact.saveDetail(&email);

        return contact;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QCoreApplication::setLibraryPaths(QStringList() << QT_PLUGINS_BINARY_DIR);
        QTest::qWait(1000);
    }

    void init()
    {
        m_manager = new QContactManager("galera");
    }

    void cleanup()
    {
        delete m_manager;
    }

    /*
     * Test create a new addressbook
     */
    void testCreateAddressBook()
    {
        QContact c;
        c.setType(QContactType::TypeGroup);
        QContactDisplayLabel label;
        QString uniqueName = QString("source@%1").arg(QUuid::createUuid().toString().remove("{").remove("}"));
        label.setLabel(uniqueName);
        c.saveDetail(&label);

        bool saveResult = m_manager->saveContact(&c);
        QVERIFY(saveResult);

        QContactDetailFilter filter;
        filter.setDetailType(QContactDetail::TypeType, QContactType::FieldType);
        filter.setValue(QContactType::TypeGroup);

        QList<QContact> contacts = m_manager->contacts(filter);
        Q_FOREACH(const QContact &contact, contacts) {
            if ((contact.detail<QContactDisplayLabel>().label() == uniqueName) &&
                (contact.id() == c.id())) {
                return;
            }
        }
        QFAIL("New source not found");
    }

    /*
     * Test remove an address book
     */
    void testRemoveAddressBook()
    {
        // create a source
        QContact c;
        c.setType(QContactType::TypeGroup);
        QContactDisplayLabel label;
        QString uniqueName = QString("source@%1").arg(QUuid::createUuid().toString().remove("{").remove("}"));
        label.setLabel(uniqueName);
        c.saveDetail(&label);

        bool saveResult = m_manager->saveContact(&c);
        QVERIFY(saveResult);

        // try to remove new source
        // WORKAROUND: Since qcontacts does not cotains a API to remove address book we use the contact label as id
        // for addressbook. This Id must contain a "@" to be handled as address book name.
        QContactId addressBookId = QContactId::fromString(QString("qtcontacts:galera::%1").arg(uniqueName));
        bool removeResult = m_manager->removeContact(addressBookId);
        QVERIFY(removeResult);

        // check if the source was removed
        QContactDetailFilter filter;
        filter.setDetailType(QContactDetail::TypeType, QContactType::FieldType);
        filter.setValue(QContactType::TypeGroup);
        QList<QContact> contacts = m_manager->contacts(filter);
        Q_FOREACH(const QContact &contact, contacts) {
            if (contact.id() == c.id()) {
                QFAIL("Source not removed");
            }
        }
    }

    /*
     * Test query a contact source using the contact group
     */
    void testQueryAddressBook()
    {
        // filter all contact groups/addressbook
        QContactDetailFilter filter;
        filter.setDetailType(QContactDetail::TypeType, QContactType::FieldType);
        filter.setValue(QContactType::TypeGroup);

        // check result for the default source
        QList<QContact> contacts = m_manager->contacts(filter);
        Q_FOREACH(const QContact &c, contacts) {
            QCOMPARE(c.type(), QContactType::TypeGroup);
            if (c.id().toString() == QStringLiteral("qtcontacts:galera::system-address-book")) {
                QContactDisplayLabel label = c.detail(QContactDisplayLabel::Type);
                QCOMPARE(label.label(), QStringLiteral("Personal"));
                return;
            }
        }
        QFAIL("Fail to create new source");
    }
};

QTEST_MAIN(AddressBookTest)

#include "address-book-test.moc"

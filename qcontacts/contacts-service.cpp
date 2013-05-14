#include "contacts-service.h"

#include "qcontact-engineid.h"
#include "vcard/vcard-parser.h"

#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusPendingReply>

#include <QtContacts/QContactChangeSet>
#include <QtContacts/QContactName>
#include <QtContacts/QContactPhoneNumber>

#include <QtVersit/QVersitReader>
#include <QtVersit/QVersitContactImporter>
#include <QtVersit/QVersitContactExporter>
#include <QtVersit/QVersitWriter>

#define GALERA_SERVICE_NAME             "com.canonical.galera"
#define GALERA_ADDRESSBOOK_OBJECT_PATH  "/com/canonical/galera/AddressBook"
#define GALERA_ADDRESSBOOK_IFACE_NAME   "com.canonical.galera.AddressBook"
#define GALERA_VIEW_IFACE_NAME          "com.canonical.galera.View"

#define FETCH_PAGE_SIZE                 30

using namespace QtVersit;


namespace galera
{

class RequestData
{
public:
    QtContacts::QContactAbstractRequest *m_request;
    QDBusInterface *m_view;
    QDBusPendingCallWatcher *m_watcher;
    int m_offset;

    RequestData()
        : m_request(0), m_view(0), m_watcher(0), m_offset(0)
    {}

    ~RequestData()
    {
        m_request = 0;
        if (m_watcher) {
            m_watcher->deleteLater();
        }
        if (m_view) {
            QDBusMessage ret = m_view->call("close");
            m_view->deleteLater();
        }
    }
};

GaleraContactsService::GaleraContactsService(const QString &managerUri)
    : m_selfContactId(),
      m_nextContactId(1),
      m_managerUri(managerUri)

{
    m_iface = QSharedPointer<QDBusInterface>(new QDBusInterface(GALERA_SERVICE_NAME,
                                                                 GALERA_ADDRESSBOOK_OBJECT_PATH,
                                                                 GALERA_ADDRESSBOOK_IFACE_NAME));
}

GaleraContactsService::GaleraContactsService(const GaleraContactsService &other)
    : m_selfContactId(other.m_selfContactId),
      m_nextContactId(other.m_nextContactId),
      m_iface(other.m_iface),
      m_managerUri(other.m_managerUri)
{
}

GaleraContactsService::~GaleraContactsService()
{
}

void GaleraContactsService::fetchContacts(QtContacts::QContactFetchRequest *request)
{
    qDebug() << Q_FUNC_INFO;
    //QContactFetchRequest *r = static_cast<QContactFetchRequest*>(request);
    //QContactFilter filter = r->filter();
    //QList<QContactSortOrder> sorting = r->sorting();
    //QContactFetchHint fetchHint = r->fetchHint();
    //QContactManager::Error operationError = QContactManager::NoError;

    QDBusMessage result = m_iface->call("query", "", "", QStringList());
    if (result.type() == QDBusMessage::ErrorMessage) {
        qWarning() << result.errorName() << result.errorMessage();
        QContactManagerEngine::updateContactFetchRequest(request, QList<QContact>(),
                                                         QContactManager::UnspecifiedError,
                                                         QContactAbstractRequest::FinishedState);
        return;
    }
    RequestData *requestData = new RequestData;
    QDBusObjectPath viewObjectPath = result.arguments()[0].value<QDBusObjectPath>();
    requestData->m_view = new QDBusInterface(GALERA_SERVICE_NAME,
                                           viewObjectPath.path(),
                                           GALERA_VIEW_IFACE_NAME);

    requestData->m_request = request;
    m_pendingRequests << requestData;
    fetchContactsPage(requestData);
}

void GaleraContactsService::fetchContactsPage(RequestData *request)
{
    qDebug() << Q_FUNC_INFO << request->m_offset;
    // Load contacs async
    QDBusPendingCall pcall = request->m_view->asyncCall("contactsDetails", QStringList(), request->m_offset, FETCH_PAGE_SIZE);
    if (pcall.isError()) {
        qWarning() << pcall.error().name() << pcall.error().message();
        QContactManagerEngine::updateContactFetchRequest(static_cast<QContactFetchRequest*>(request->m_request), QList<QContact>(),
                                                         QContactManager::UnspecifiedError,
                                                         QContactAbstractRequest::FinishedState);
        destroyRequest(request);
        return;
    }
    request->m_watcher = new QDBusPendingCallWatcher(pcall, 0);
    QObject::connect(request->m_watcher, &QDBusPendingCallWatcher::finished,
                     [=](QDBusPendingCallWatcher *call) {
                        this->fetchContactsDone(request, call);
                     });
}

void GaleraContactsService::fetchContactsDone(RequestData *request, QDBusPendingCallWatcher *call)
{
    qDebug() << Q_FUNC_INFO;
    QDBusPendingReply<QStringList> reply = *call;
    if (reply.isError()) {
        qWarning() << reply.error().name() << reply.error().message();
        QContactManagerEngine::updateContactFetchRequest(static_cast<QContactFetchRequest*>(request->m_request),
                                                         QList<QContact>(),
                                                         QContactManager::UnspecifiedError,
                                                         QContactAbstractRequest::FinishedState);
        destroyRequest(request);
    } else {
        QList<QContact> contacts;
        const QStringList vcards = reply.value();
        QContactAbstractRequest::State opState = QContactAbstractRequest::FinishedState;

        if (vcards.size() == FETCH_PAGE_SIZE) {
            opState = QContactAbstractRequest::ActiveState;
        }

        if (!vcards.isEmpty()) {
            contacts = VCardParser::vcardToContact(vcards);
            QList<QContactId> contactsIds;

            QList<QContact>::iterator contact;
            for (contact = contacts.begin(); contact != contacts.end(); ++contact) {
                if (!contact->isEmpty()) {
                    quint32 nextId = m_nextContactId;

                    QContactGuid detailId = contact->detail<QContactGuid>();
                    GaleraEngineId *engineId = new GaleraEngineId(detailId.guid(), m_managerUri);
                    QContactId newId = QContactId(engineId);

                    contact->setId(newId);
                    contactsIds << newId;
                    m_nextContactId = nextId;

#if 0
                    if (m_nextContactId < 10) {
                        qDebug() << "VCARD" << vcards.at(m_nextContactId-1);
                        qDebug() << "Imported contact" << contact->id()
                                 << "Name:" << contact->detail<QContactName>().firstName()
                                 << "Phone" << contact->detail<QContactPhoneNumber>().number();
                        Q_FOREACH(QContactDetail det, contact->details()) {
                            qDebug() << det;
                        }
                    }
#endif
                }
            }

            m_contacts += contacts;
            m_contactIds += contactsIds;
        }

        QContactManagerEngine::updateContactFetchRequest(static_cast<QContactFetchRequest*>(request->m_request),
                                                         m_contacts,
                                                         QContactManager::NoError,
                                                         opState);

        if (opState == QContactAbstractRequest::ActiveState) {
            request->m_offset += FETCH_PAGE_SIZE;
            request->m_watcher->deleteLater();
            request->m_watcher = 0;
            fetchContactsPage(request);
        } else {
            destroyRequest(request);
        }
    }
}

void GaleraContactsService::saveContact(QtContacts::QContactSaveRequest *request)
{
    qDebug() << Q_FUNC_INFO;
    //QContactFetchRequest *r = static_cast<QContactFetchRequest*>(request);
    //QContactFilter filter = r->filter();
    //QList<QContactSortOrder> sorting = r->sorting();
    //QContactFetchHint fetchHint = r->fetchHint();
    //QContactManager::Error operationError = QContactManager::NoError;

    QStringList vcards;
    QList<QContact> contacts = request->contacts();
    QVersitContactExporter contactExporter;
    if (!contactExporter.exportContacts(contacts, QVersitDocument::VCard30Type)) {
        qDebug() << "Fail to export contacts" << contactExporter.errors();
        //TODO : set error
        return;
    }

    QByteArray vcard;
    Q_FOREACH(QVersitDocument doc, contactExporter.documents()) {
        vcard.clear();
        QVersitWriter writer(&vcard);
        if (!writer.startWriting(doc)) {
            qWarning() << "Fail to write contacts" << doc;
        } else {
            writer.waitForFinished();
            vcards << QString::fromUtf8(vcard);
        }
    }

    QStringList oldContacts;
    QStringList newContacts;
    for(int i=0, iMax=contacts.size(); i < iMax; i++) {
        if (contacts.at(i).id().isNull()) {
            newContacts << vcards[i];
        } else {
            oldContacts << vcards[i];
        }
    }

    if (!oldContacts.isEmpty()) {
        updateContacts(request, oldContacts);
    }
    if (!newContacts.isEmpty()) {
        createContacts(request, newContacts);
    }
}
void GaleraContactsService::createContacts(QtContacts::QContactSaveRequest *request, QStringList &contacts)
{
    qDebug() << Q_FUNC_INFO;
    if (contacts.count() > 1) {
        qWarning() << "TODO: implement contact creation support to more then one contact.";
        return;
    }

    Q_FOREACH(QString contact, contacts) {
        QDBusPendingCall pcall = m_iface->asyncCall("createContact", contact, "");
        RequestData *requestData = new RequestData;
        requestData->m_request = request;
        requestData->m_watcher = new QDBusPendingCallWatcher(pcall, 0);
        QObject::connect(requestData->m_watcher, &QDBusPendingCallWatcher::finished,
                         [=](QDBusPendingCallWatcher *call) {
                            this->createContactsDone(requestData, call);
                         });
    }
}

void GaleraContactsService::createContactsDone(RequestData *request, QDBusPendingCallWatcher *call)
{
    qDebug() << Q_FUNC_INFO;
    QDBusPendingReply<QString> reply = *call;
    QList<QContact> contacts;
    QMap<int, QContactManager::Error> saveError;
    QContactManager::Error opError = QContactManager::NoError;

    if (reply.isError()) {
        qWarning() << reply.error().name() << reply.error().message();
        opError = QContactManager::UnspecifiedError;
    } else {
        const QString contactId = reply.value();
        if (!contactId.isEmpty()) {
            contacts = static_cast<QtContacts::QContactSaveRequest *>(request->m_request)->contacts();
            GaleraEngineId *engineId = new GaleraEngineId(contactId, "galera");
            QContactId contactId(engineId);
            contacts[0].setId(QContactId(contactId));
        }
    }

    QContactManagerEngine::updateContactSaveRequest(static_cast<QContactSaveRequest*>(request->m_request),
                                                    contacts,
                                                    opError,
                                                    saveError,
                                                    QContactAbstractRequest::FinishedState);
    destroyRequest(request);
}

void GaleraContactsService::updateContacts(QtContacts::QContactSaveRequest *request, QStringList &contacts)
{
    qDebug() << Q_FUNC_INFO;
    QDBusPendingCall pcall = m_iface->asyncCall("updateContacts", contacts);
    if (pcall.isError()) {
        qWarning() <<  "Error" << pcall.error().name() << pcall.error().message();
        QContactManagerEngine::updateContactSaveRequest(request,
                                                        QList<QContact>(),
                                                        QContactManager::UnspecifiedError,
                                                        QMap<int, QContactManager::Error>(),
                                                        QContactAbstractRequest::FinishedState);
    } else {
        qDebug() << "Wait for save contact";
        RequestData *requestData = new RequestData;
        requestData->m_request = request;
        requestData->m_watcher = new QDBusPendingCallWatcher(pcall, 0);
        QObject::connect(requestData->m_watcher, &QDBusPendingCallWatcher::finished,
                         [=](QDBusPendingCallWatcher *call) {
                            this->updateContactDone(requestData, call);
                         });
    }
}


void GaleraContactsService::updateContactDone(RequestData *request, QDBusPendingCallWatcher *call)
{
    qDebug() << Q_FUNC_INFO;
    QDBusPendingReply<QStringList> reply = *call;
    QList<QContact> contacts;
    QMap<int, QContactManager::Error> saveError;
    QContactManager::Error opError = QContactManager::NoError;

    if (reply.isError()) {
        qWarning() << reply.error().name() << reply.error().message();
        opError = QContactManager::UnspecifiedError;
    } else {
        const QStringList vcards = reply.value();
        if (!vcards.isEmpty()) {
            QMap<int, QVersitContactImporter::Error> importErrors;
            //TODO report parse errors
            contacts = VCardParser::vcardToContact(vcards);
            Q_FOREACH(int key, importErrors.keys()) {
                saveError.insert(key, QContactManager::BadArgumentError);
            }
        }
    }

    QContactManagerEngine::updateContactSaveRequest(static_cast<QContactSaveRequest*>(request->m_request),
                                                    contacts,
                                                    opError,
                                                    saveError,
                                                    QContactAbstractRequest::FinishedState);
    destroyRequest(request);
}

void GaleraContactsService::addRequest(QtContacts::QContactAbstractRequest *request)
{
    qDebug() << Q_FUNC_INFO << request->state();
    Q_ASSERT(request->state() == QContactAbstractRequest::ActiveState);
    switch (request->type()) {
        case QContactAbstractRequest::ContactFetchRequest:
            fetchContacts(static_cast<QContactFetchRequest*>(request));
            break;
        case QContactAbstractRequest::ContactFetchByIdRequest:
            break;
        case QContactAbstractRequest::ContactIdFetchRequest:
            break;
        case QContactAbstractRequest::ContactSaveRequest:
            saveContact(static_cast<QContactSaveRequest*>(request));
            break;
        case QContactAbstractRequest::ContactRemoveRequest:
        case QContactAbstractRequest::RelationshipFetchRequest:
        case QContactAbstractRequest::RelationshipRemoveRequest:
        case QContactAbstractRequest::RelationshipSaveRequest:
        break;

        default: // unknown request type.
        break;
    }
}

void GaleraContactsService::destroyRequest(RequestData *request)
{
    qDebug() << Q_FUNC_INFO;
    m_pendingRequests.remove(request);
    delete request;
}

} //namespace
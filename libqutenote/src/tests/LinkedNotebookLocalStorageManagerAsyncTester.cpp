#include "LinkedNotebookLocalStorageManagerAsyncTester.h"
#include <qute_note/local_storage/LocalStorageManagerThreadWorker.h>
#include <qute_note/logging/QuteNoteLogger.h>
#include <QThread>

namespace qute_note {
namespace test {

LinkedNotebookLocalStorageManagerAsyncTester::LinkedNotebookLocalStorageManagerAsyncTester(QObject * parent) :
    QObject(parent),
    m_state(STATE_UNINITIALIZED),
    m_pLocalStorageManagerThreadWorker(Q_NULLPTR),
    m_pLocalStorageManagerThread(Q_NULLPTR),
    m_initialLinkedNotebook(),
    m_foundLinkedNotebook(),
    m_modifiedLinkedNotebook(),
    m_initialLinkedNotebooks()
{}

LinkedNotebookLocalStorageManagerAsyncTester::~LinkedNotebookLocalStorageManagerAsyncTester()
{
    // NOTE: shouldn't attempt to delete m_pLocalStorageManagerThread as Qt's parent-child system
    // should take care of that

    if (m_pLocalStorageManagerThread) {
        m_pLocalStorageManagerThread->quit();
        m_pLocalStorageManagerThread->wait();
    }

    if (m_pLocalStorageManagerThreadWorker) {
        delete m_pLocalStorageManagerThreadWorker;
    }
}

void LinkedNotebookLocalStorageManagerAsyncTester::onInitTestCase()
{
    QString username = "LinkedNotebookLocalStorageManagerAsyncTester";
    qint32 userId = 1;
    bool startFromScratch = true;
    bool overrideLock = false;

    if (m_pLocalStorageManagerThread) {
        delete m_pLocalStorageManagerThread;
        m_pLocalStorageManagerThread = Q_NULLPTR;
    }

    if (m_pLocalStorageManagerThreadWorker) {
        delete m_pLocalStorageManagerThreadWorker;
        m_pLocalStorageManagerThreadWorker = Q_NULLPTR;
    }

    m_state = STATE_UNINITIALIZED;

    m_pLocalStorageManagerThread = new QThread(this);
    m_pLocalStorageManagerThreadWorker = new LocalStorageManagerThreadWorker(username, userId, startFromScratch, overrideLock);
    m_pLocalStorageManagerThreadWorker->moveToThread(m_pLocalStorageManagerThread);

    createConnections();

    m_pLocalStorageManagerThread->start();
}

void LinkedNotebookLocalStorageManagerAsyncTester::onWorkerInitialized()
{
    m_initialLinkedNotebook.setGuid("00000000-0000-0000-c000-000000000001");
    m_initialLinkedNotebook.setUpdateSequenceNumber(1);
    m_initialLinkedNotebook.setShareName("Fake linked notebook share name");
    m_initialLinkedNotebook.setUsername("Fake linked notebook username");
    m_initialLinkedNotebook.setShardId("Fake linked notebook shard id");
    m_initialLinkedNotebook.setShareKey("Fake linked notebook share key");
    m_initialLinkedNotebook.setUri("Fake linked notebook uri");
    m_initialLinkedNotebook.setNoteStoreUrl("Fake linked notebook note store url");
    m_initialLinkedNotebook.setWebApiUrlPrefix("Fake linked notebook web api url prefix");
    m_initialLinkedNotebook.setStack("Fake linked notebook stack");
    m_initialLinkedNotebook.setBusinessId(1);

    QString errorDescription;
    if (!m_initialLinkedNotebook.checkParameters(errorDescription)) {
        QNWARNING("Found invalid LinkedNotebook: " << m_initialLinkedNotebook << ", error: "
                  << errorDescription);
        emit failure(errorDescription);
        return;
    }

    m_state = STATE_SENT_ADD_REQUEST;
    emit addLinkedNotebookRequest(m_initialLinkedNotebook);
}

void LinkedNotebookLocalStorageManagerAsyncTester::onGetLinkedNotebookCountCompleted(int count, QUuid requestId)
{
    Q_UNUSED(requestId)

    QString errorDescription;

#define HANDLE_WRONG_STATE() \
    else { \
        errorDescription = "Internal error in LinkedNotebookLocalStorageManagerAsyncTester: " \
                           "found wrong state"; \
        emit failure(errorDescription); \
        return; \
    }

    if (m_state == STATE_SENT_GET_COUNT_AFTER_UPDATE_REQUEST)
    {
        if (count != 1) {
            errorDescription = "GetLinkedNotebookCount returned result different from the expected one (1): ";
            errorDescription += QString::number(count);
            emit failure(errorDescription);
            return;
        }

        m_state = STATE_SENT_EXPUNGE_REQUEST;
        emit expungeLinkedNotebookRequest(m_modifiedLinkedNotebook);
    }
    else if (m_state == STATE_SENT_GET_COUNT_AFTER_EXPUNGE_REQUEST)
    {
        if (count != 0) {
            errorDescription = "GetLinkedNotebookCount returned result different from the expected one (0): ";
            errorDescription += QString::number(count);
            emit failure(errorDescription);
            return;
        }

        LinkedNotebook extraLinkedNotebook;
        extraLinkedNotebook.setGuid("00000000-0000-0000-c000-000000000001");
        extraLinkedNotebook.setUpdateSequenceNumber(1);
        extraLinkedNotebook.setUsername("Extra LinkedNotebook");
        extraLinkedNotebook.setShareName("Fake extra linked notebook share name");
        extraLinkedNotebook.setShareKey("Fake extra linked notebook share key");
        extraLinkedNotebook.setShardId("Fake extra linked notebook shard id");
        extraLinkedNotebook.setStack("Fake extra linked notebook stack");
        extraLinkedNotebook.setNoteStoreUrl("Fake extra linked notebook note store url");
        extraLinkedNotebook.setWebApiUrlPrefix("Fake extra linked notebook web api url prefix");
        extraLinkedNotebook.setUri("Fake extra linked notebook uri");

        m_state = STATE_SENT_ADD_EXTRA_LINKED_NOTEBOOK_ONE_REQUEST;
        emit addLinkedNotebookRequest(extraLinkedNotebook);
    }
    HANDLE_WRONG_STATE();
}

void LinkedNotebookLocalStorageManagerAsyncTester::onGetLinkedNotebookCountFailed(QString errorDescription, QUuid requestId)
{
    QNWARNING(errorDescription << ", requestId = " << requestId);
    emit failure(errorDescription);
}

void LinkedNotebookLocalStorageManagerAsyncTester::onAddLinkedNotebookCompleted(LinkedNotebook notebook, QUuid requestId)
{
    Q_UNUSED(requestId)

    QString errorDescription;

    if (m_state == STATE_SENT_ADD_REQUEST)
    {
        if (m_initialLinkedNotebook != notebook) {
            errorDescription = "Internal error in LinkedNotebookLocalStorageManagerAsyncTester: "
                               "notebook in addLinkedNotebookCompleted slot doesn't match the original LinkedNotebook";
            QNWARNING(errorDescription);
            emit failure(errorDescription);
            return;
        }

        m_foundLinkedNotebook = LinkedNotebook();
        m_foundLinkedNotebook.setGuid(notebook.guid());

        m_state = STATE_SENT_FIND_AFTER_ADD_REQUEST;
        emit findLinkedNotebookRequest(m_foundLinkedNotebook);
    }
    else if (m_state == STATE_SENT_ADD_EXTRA_LINKED_NOTEBOOK_ONE_REQUEST)
    {
        m_initialLinkedNotebooks << notebook;

        LinkedNotebook extraLinkedNotebook;
        extraLinkedNotebook.setGuid("00000000-0000-0000-c000-000000000002");
        extraLinkedNotebook.setUpdateSequenceNumber(2);
        extraLinkedNotebook.setUsername("Fake linked notebook username two");
        extraLinkedNotebook.setShareName("Fake extra linked notebook share name two");
        extraLinkedNotebook.setShareKey("Fake extra linked notebook share key two");
        extraLinkedNotebook.setShardId("Fake extra linked notebook shard id two");
        extraLinkedNotebook.setStack("Fake extra linked notebook stack two");
        extraLinkedNotebook.setNoteStoreUrl("Fake extra linked notebook note store url two");
        extraLinkedNotebook.setWebApiUrlPrefix("Fake extra linked notebook web api url prefix two");
        extraLinkedNotebook.setUri("Fake extra linked notebook uri two");

        m_state = STATE_SENT_ADD_EXTRA_LINKED_NOTEBOOK_TWO_REQUEST;
        emit addLinkedNotebookRequest(extraLinkedNotebook);
    }
    else if (m_state == STATE_SENT_ADD_EXTRA_LINKED_NOTEBOOK_TWO_REQUEST)
    {
        m_initialLinkedNotebooks << notebook;

        m_state = STATE_SENT_LIST_LINKED_NOTEBOOKS_REQUEST;
        size_t limit = 0, offset = 0;
        LocalStorageManager::ListLinkedNotebooksOrder::type order = LocalStorageManager::ListLinkedNotebooksOrder::NoOrder;
        LocalStorageManager::OrderDirection::type orderDirection = LocalStorageManager::OrderDirection::Ascending;
        emit listAllLinkedNotebooksRequest(limit, offset, order, orderDirection);
    }
    HANDLE_WRONG_STATE();
}

void LinkedNotebookLocalStorageManagerAsyncTester::onAddLinkedNotebookFailed(LinkedNotebook notebook,
                                                                             QString errorDescription, QUuid requestId)
{
    QNWARNING(errorDescription << ", requestId = " << requestId << ", linked notebook: " << notebook);
    emit failure(errorDescription);
}

void LinkedNotebookLocalStorageManagerAsyncTester::onUpdateLinkedNotebookCompleted(LinkedNotebook notebook, QUuid requestId)
{
    Q_UNUSED(requestId)

    QString errorDescription;

    if (m_state == STATE_SENT_UPDATE_REQUEST)
    {
        if (m_modifiedLinkedNotebook != notebook) {
            errorDescription = "Internal error in LinkedNotebookLocalStorageManagerAsyncTester: "
                               "notebook in onUpdateLinkedNotebookCompleted slot doesn't match "
                               "the original modified LinkedNotebook";
            QNWARNING(errorDescription);
            emit failure(errorDescription);
            return;
        }

        m_state = STATE_SENT_FIND_AFTER_UPDATE_REQUEST;
        emit findLinkedNotebookRequest(m_foundLinkedNotebook);
    }
}

void LinkedNotebookLocalStorageManagerAsyncTester::onUpdateLinkedNotebookFailed(LinkedNotebook notebook, QString errorDescription, QUuid requestId)
{
    QNWARNING(errorDescription << ", requestId = " << requestId << ", linked notebook: " << notebook);
    emit failure(errorDescription);
}

void LinkedNotebookLocalStorageManagerAsyncTester::onFindLinkedNotebookCompleted(LinkedNotebook notebook, QUuid requestId)
{
    Q_UNUSED(requestId)

    QString errorDescription;

    if (m_state == STATE_SENT_FIND_AFTER_ADD_REQUEST)
    {
        if (notebook != m_initialLinkedNotebook) {
            errorDescription = "Added and found linked notebooks in local storage don't match";
            QNWARNING(errorDescription << ": LinkedNotebook added to LocalStorageManager: " << m_initialLinkedNotebook
                      << "\nLinkedNotebook found in LocalStorageManager: " << notebook);
            emit failure(errorDescription);
            return;
        }

        // Ok, found linked notebook is good, updating it now
        m_modifiedLinkedNotebook = m_initialLinkedNotebook;
        m_modifiedLinkedNotebook.setUpdateSequenceNumber(m_initialLinkedNotebook.updateSequenceNumber() + 1);
        m_modifiedLinkedNotebook.setUsername(m_initialLinkedNotebook.username() + "_modified");
        m_modifiedLinkedNotebook.setStack(m_initialLinkedNotebook.stack() + "_modified");
        m_modifiedLinkedNotebook.setShareName(m_initialLinkedNotebook.shareName() + "_modified");

        m_state = STATE_SENT_UPDATE_REQUEST;
        emit updateLinkedNotebookRequest(m_modifiedLinkedNotebook);
    }
    else if (m_state == STATE_SENT_FIND_AFTER_UPDATE_REQUEST)
    {
        if (notebook != m_modifiedLinkedNotebook) {
            errorDescription = "Updated and found linked notebooks in local storage don't match";
            QNWARNING(errorDescription << ": LinkedNotebook updated in LocalStorageManager: "
                      << m_modifiedLinkedNotebook << "\nLinkedNotebook found in LocalStorageManager: "
                      << notebook);
            emit failure(errorDescription);
            return;
        }

        m_state = STATE_SENT_GET_COUNT_AFTER_UPDATE_REQUEST;
        emit getLinkedNotebookCountRequest();
    }
    else if (m_state == STATE_SENT_FIND_AFTER_EXPUNGE_REQUEST)
    {
        errorDescription = "Error: found linked notebook which should have been expunged from local storage";
        QNWARNING(errorDescription << ": LinkedNotebook expunged from LocalStorageManager: " << m_modifiedLinkedNotebook
                  << "\nLinkedNotebook found in LocalStorageManager: " << notebook);
        emit failure(errorDescription);
        return;
    }
    HANDLE_WRONG_STATE();
}

void LinkedNotebookLocalStorageManagerAsyncTester::onFindLinkedNotebookFailed(LinkedNotebook notebook, QString errorDescription, QUuid requestId)
{
    if (m_state == STATE_SENT_FIND_AFTER_EXPUNGE_REQUEST) {
        m_state = STATE_SENT_GET_COUNT_AFTER_EXPUNGE_REQUEST;
        emit getLinkedNotebookCountRequest();
        return;
    }

    QNWARNING(errorDescription << ", requestId = " << requestId << ", linked notebook: " << notebook);
    emit failure(errorDescription);
}

void LinkedNotebookLocalStorageManagerAsyncTester::onListAllLinkedNotebooksCompleted(size_t limit, size_t offset,
                                                                                     LocalStorageManager::ListLinkedNotebooksOrder::type order,
                                                                                     LocalStorageManager::OrderDirection::type orderDirection, QList<LinkedNotebook> linkedNotebooks,
                                                                                     QUuid requestId)
{
    Q_UNUSED(limit)
    Q_UNUSED(offset)
    Q_UNUSED(order)
    Q_UNUSED(orderDirection)
    Q_UNUSED(requestId)

    int numInitialLinkedNotebooks = m_initialLinkedNotebooks.size();
    int numFoundLinkedNotebooks = linkedNotebooks.size();

    QString errorDescription;

    if (numInitialLinkedNotebooks != numFoundLinkedNotebooks) {
        errorDescription = "Error: number of found linked notebooks does not correspond "
                           "to the number of original added linked notebooks";
        emit failure(errorDescription);
        return;
    }

    foreach(const LinkedNotebook & notebook, m_initialLinkedNotebooks)
    {
        if (!linkedNotebooks.contains(notebook)) {
            errorDescription = "Error: one of initial linked notebooks was not found "
                               "within found linked notebooks";
            emit failure(errorDescription);
            return;
        }
    }

    emit success();
}

void LinkedNotebookLocalStorageManagerAsyncTester::onListAllLinkedNotebooksFailed(size_t limit, size_t offset,
                                                                                  LocalStorageManager::ListLinkedNotebooksOrder::type order,
                                                                                  LocalStorageManager::OrderDirection::type orderDirection,
                                                                                  QString errorDescription, QUuid requestId)
{
    Q_UNUSED(limit)
    Q_UNUSED(offset)
    Q_UNUSED(order)
    Q_UNUSED(orderDirection)

    QNWARNING(errorDescription << ", requestId = " << requestId);
    emit failure(errorDescription);
}

void LinkedNotebookLocalStorageManagerAsyncTester::onExpungeLinkedNotebookCompleted(LinkedNotebook notebook, QUuid requestId)
{
    Q_UNUSED(requestId)

    QString errorDescription;

    if (m_modifiedLinkedNotebook != notebook) {
        errorDescription = "Internal error in LinkedNotebookLocalStorageManagerAsyncTester: "
                           "linked notebook in onExpungeLinkedNotebookCompleted slot doesn't match "
                           "the original expunged LinkedNotebook";
        QNWARNING(errorDescription);
        emit failure(errorDescription);
        return;
    }

    m_state = STATE_SENT_FIND_AFTER_EXPUNGE_REQUEST;
    emit findLinkedNotebookRequest(m_foundLinkedNotebook);
}

void LinkedNotebookLocalStorageManagerAsyncTester::onExpungeLinkedNotebookFailed(LinkedNotebook notebook,
                                                                                 QString errorDescription, QUuid requestId)
{
    QNWARNING(errorDescription << ", requestId = " << requestId << ", linked notebook: " << notebook);
    emit failure(errorDescription);
}

void LinkedNotebookLocalStorageManagerAsyncTester::createConnections()
{
    QObject::connect(m_pLocalStorageManagerThreadWorker, QNSIGNAL(LocalStorageManagerThreadWorker,failure,QString),
                     this, QNSIGNAL(LinkedNotebookLocalStorageManagerAsyncTester,failure,QString));

    QObject::connect(m_pLocalStorageManagerThread, QNSIGNAL(QThread,started), m_pLocalStorageManagerThreadWorker, QNSLOT(LocalStorageManagerThreadWorker,init));
    QObject::connect(m_pLocalStorageManagerThread, QNSIGNAL(QThread,finished), m_pLocalStorageManagerThread, QNSLOT(QThread,deleteLater));

    QObject::connect(m_pLocalStorageManagerThreadWorker, QNSIGNAL(LocalStorageManagerThreadWorker,initialized), this, QNSLOT(LinkedNotebookLocalStorageManagerAsyncTester,onWorkerInitialized));

    // Request --> slot connections
    QObject::connect(this, QNSIGNAL(LinkedNotebookLocalStorageManagerAsyncTester,getLinkedNotebookCountRequest,QUuid),
                     m_pLocalStorageManagerThreadWorker, QNSLOT(LocalStorageManagerThreadWorker,onGetLinkedNotebookCountRequest,QUuid));
    QObject::connect(this, QNSIGNAL(LinkedNotebookLocalStorageManagerAsyncTester,addLinkedNotebookRequest,LinkedNotebook,QUuid),
                     m_pLocalStorageManagerThreadWorker, QNSLOT(LocalStorageManagerThreadWorker,onAddLinkedNotebookRequest,LinkedNotebook,QUuid));
    QObject::connect(this, QNSIGNAL(LinkedNotebookLocalStorageManagerAsyncTester,updateLinkedNotebookRequest,LinkedNotebook,QUuid),
                     m_pLocalStorageManagerThreadWorker, QNSLOT(LocalStorageManagerThreadWorker,onUpdateLinkedNotebookRequest,LinkedNotebook,QUuid));
    QObject::connect(this, QNSIGNAL(LinkedNotebookLocalStorageManagerAsyncTester,findLinkedNotebookRequest,LinkedNotebook,QUuid),
                     m_pLocalStorageManagerThreadWorker, QNSLOT(LocalStorageManagerThreadWorker,onFindLinkedNotebookRequest,LinkedNotebook,QUuid));
    QObject::connect(this,
                     QNSIGNAL(LinkedNotebookLocalStorageManagerAsyncTester,listAllLinkedNotebooksRequest,size_t,size_t,
                              LocalStorageManager::ListLinkedNotebooksOrder::type,LocalStorageManager::OrderDirection::type,QUuid),
                     m_pLocalStorageManagerThreadWorker,
                     QNSLOT(LocalStorageManagerThreadWorker,onListAllLinkedNotebooksRequest,size_t,size_t,
                            LocalStorageManager::ListLinkedNotebooksOrder::type,
                            LocalStorageManager::OrderDirection::type,QUuid));
    QObject::connect(this, QNSIGNAL(LinkedNotebookLocalStorageManagerAsyncTester,expungeLinkedNotebookRequest,LinkedNotebook,QUuid),
                     m_pLocalStorageManagerThreadWorker, QNSLOT(LocalStorageManagerThreadWorker,onExpungeLinkedNotebookRequest,LinkedNotebook,QUuid));

    // Slot <-- result connections
    QObject::connect(m_pLocalStorageManagerThreadWorker, QNSIGNAL(LocalStorageManagerThreadWorker,getLinkedNotebookCountComplete,int,QUuid),
                     this, QNSLOT(LinkedNotebookLocalStorageManagerAsyncTester,onGetLinkedNotebookCountCompleted,int,QUuid));
    QObject::connect(m_pLocalStorageManagerThreadWorker, QNSIGNAL(LocalStorageManagerThreadWorker,getLinkedNotebookCountFailed,QString,QUuid),
                     this, QNSLOT(LinkedNotebookLocalStorageManagerAsyncTester,onGetLinkedNotebookCountFailed,QString,QUuid));
    QObject::connect(m_pLocalStorageManagerThreadWorker, QNSIGNAL(LocalStorageManagerThreadWorker,addLinkedNotebookComplete,LinkedNotebook,QUuid),
                     this, QNSLOT(LinkedNotebookLocalStorageManagerAsyncTester,onAddLinkedNotebookCompleted,LinkedNotebook,QUuid));
    QObject::connect(m_pLocalStorageManagerThreadWorker, QNSIGNAL(LocalStorageManagerThreadWorker,addLinkedNotebookFailed,LinkedNotebook,QString,QUuid),
                     this, QNSLOT(LinkedNotebookLocalStorageManagerAsyncTester,onAddLinkedNotebookFailed,LinkedNotebook,QString,QUuid));
    QObject::connect(m_pLocalStorageManagerThreadWorker, QNSIGNAL(LocalStorageManagerThreadWorker,updateLinkedNotebookComplete,LinkedNotebook,QUuid),
                     this, QNSLOT(LinkedNotebookLocalStorageManagerAsyncTester,onUpdateLinkedNotebookCompleted,LinkedNotebook,QUuid));
    QObject::connect(m_pLocalStorageManagerThreadWorker, QNSIGNAL(LocalStorageManagerThreadWorker,updateLinkedNotebookFailed,LinkedNotebook,QString,QUuid),
                     this, QNSLOT(LinkedNotebookLocalStorageManagerAsyncTester,onUpdateLinkedNotebookFailed,LinkedNotebook,QString,QUuid));
    QObject::connect(m_pLocalStorageManagerThreadWorker, QNSIGNAL(LocalStorageManagerThreadWorker,findLinkedNotebookComplete,LinkedNotebook,QUuid),
                     this, QNSLOT(LinkedNotebookLocalStorageManagerAsyncTester,onFindLinkedNotebookCompleted,LinkedNotebook,QUuid));
    QObject::connect(m_pLocalStorageManagerThreadWorker, QNSIGNAL(LocalStorageManagerThreadWorker,findLinkedNotebookFailed,LinkedNotebook,QString,QUuid),
                     this, QNSLOT(LinkedNotebookLocalStorageManagerAsyncTester,onFindLinkedNotebookFailed,LinkedNotebook,QString,QUuid));
    QObject::connect(m_pLocalStorageManagerThreadWorker,
                     QNSIGNAL(LocalStorageManagerThreadWorker,listAllLinkedNotebooksComplete,size_t,size_t,
                              LocalStorageManager::ListLinkedNotebooksOrder::type,LocalStorageManager::OrderDirection::type,
                              QList<LinkedNotebook>,QUuid),
                     this,
                     QNSLOT(LinkedNotebookLocalStorageManagerAsyncTester,onListAllLinkedNotebooksCompleted,size_t,size_t,
                            LocalStorageManager::ListLinkedNotebooksOrder::type,LocalStorageManager::OrderDirection::type,
                            QList<LinkedNotebook>,QUuid));
    QObject::connect(m_pLocalStorageManagerThreadWorker,
                     QNSIGNAL(LocalStorageManagerThreadWorker,listAllLinkedNotebooksFailed,size_t,size_t,
                              LocalStorageManager::ListLinkedNotebooksOrder::type,LocalStorageManager::OrderDirection::type,
                              QString,QUuid),
                     this,
                     QNSLOT(LinkedNotebookLocalStorageManagerAsyncTester,onListAllLinkedNotebooksFailed,size_t,size_t,
                            LocalStorageManager::ListLinkedNotebooksOrder::type,LocalStorageManager::OrderDirection::type,
                            QString,QUuid));
    QObject::connect(m_pLocalStorageManagerThreadWorker, QNSIGNAL(LocalStorageManagerThreadWorker,expungeLinkedNotebookComplete,LinkedNotebook,QUuid),
                     this, QNSLOT(LinkedNotebookLocalStorageManagerAsyncTester,onExpungeLinkedNotebookCompleted,LinkedNotebook,QUuid));
    QObject::connect(m_pLocalStorageManagerThreadWorker, QNSIGNAL(LocalStorageManagerThreadWorker,expungeLinkedNotebookFailed,LinkedNotebook,QString,QUuid),
                     this, QNSLOT(LinkedNotebookLocalStorageManagerAsyncTester,onExpungeLinkedNotebookFailed,LinkedNotebook,QString,QUuid));
}

#undef HANDLE_WRONG_STATE

} // namespace test
} // namespace qute_note

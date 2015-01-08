#ifndef __QUTE_NOTE__CORE__CLIENT__SYNCHRONIZATION__SYNCHRONIZATION_MANAGER_PRIVATE_H
#define __QUTE_NOTE__CORE__CLIENT__SYNCHRONIZATION__SYNCHRONIZATION_MANAGER_PRIVATE_H

#include "RemoteToLocalSynchronizationManager.h"
#include <tools/qt4helper.h>
#include <QEverCloud.h>
#include <oauth.h>
#include <QObject>

namespace qute_note {

class SynchronizationManagerPrivate: public QObject
{
    Q_OBJECT
public:
    SynchronizationManagerPrivate(LocalStorageManagerThreadWorker & localStorageManagerThreadWorker);
    virtual ~SynchronizationManagerPrivate();

    void synchronize();

Q_SIGNALS:
    void notifyError(QString errorDescription);

private Q_SLOTS:
    void onOAuthSuccess();
    void onOAuthFailure();
    void onOAuthResult(bool result);

    void onRemoteToLocalSyncFinished(qint32 lastUpdateCount, qint32 lastSyncTime);

private:
    SynchronizationManagerPrivate() Q_DECL_DELETE;
    SynchronizationManagerPrivate(const SynchronizationManagerPrivate & other) Q_DECL_DELETE;
    SynchronizationManagerPrivate & operator=(const SynchronizationManagerPrivate & other) Q_DECL_DELETE;

    void createConnections();

    void authenticate();

    void launchOAuth();

    void launchSync();
    void launchFullSync();
    void launchIncrementalSync();
    void sendChanges();

    bool storeOAuthResult();

    virtual void timerEvent(QTimerEvent * pTimerEvent);

    bool tryToGetSyncState(qevercloud::SyncState & syncState);

private:
    qint32      m_maxSyncChunkEntries;
    qint32      m_lastUpdateCount;
    qint32      m_lastSyncTime;

    NoteStore   m_noteStore;

    int         m_launchSyncPostponeTimerId;

    QScopedPointer<qevercloud::EvernoteOAuthWebView>                m_pOAuthWebView;
    QSharedPointer<qevercloud::EvernoteOAuthWebView::OAuthResult>   m_pOAuthResult;

    RemoteToLocalSynchronizationManager     m_remoteToLocalSyncManager;
};

} // namespace qute_note

#endif // __QUTE_NOTE__CORE__CLIENT__SYNCHRONIZATION__SYNCHRONIZATION_MANAGER_PRIVATE_H

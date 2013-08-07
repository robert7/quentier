#ifndef __QUTE_NOTE__CLIENT__EVERNOTE_SERVICE_MANAGER_H
#define __QUTE_NOTE__CLIENT__EVERNOTE_SERVICE_MANAGER_H

#include <QObject>
#include <QString>
#include <QUrl>
#include "CredentialsModel.h"

template <class T>
class Singleton;

QT_FORWARD_DECLARE_CLASS(EvernoteOAuthBrowser)
QT_FORWARD_DECLARE_CLASS(EvernoteServiceOAuthHandler)

class EvernoteServiceManager: public QObject
{
    Q_OBJECT
public:
    // User interface

    /**
     * @brief authenticate - attempts to receive OAuth tokens from Evernote service.
     * As a result of its job statusText signal is emitted in case of both success and failure.
     */
    void authenticate();

    /**
     * @brief connect - attempts to connect to Evernote service using
     * prespecified credentials and obtained OAuth tokens. As a result of its job
     * statusText signal is emitted in case of both success and failure.
     */
    void connect();

    /**
     * @brief disconnect - disconnects from Evernote service.
     */
    void disconnect();

    /**
     * @brief setRefreshTime - define interval in seconds needed to maintain connection
     * @param refreshTime - time to refresh connection in seconds
     */
    void setRefreshTime(const double refreshTime);

private:
    // standard constructor does nothing but exists as private method
    EvernoteServiceManager();
    friend class Singleton<EvernoteServiceManager>;

public:
    // Application interface
    static EvernoteServiceManager & Instance();

    bool setCredentials(const CredentialsModel & credentials,
                        QString & errorMessage);

    CredentialsModel & getCredentials() { return m_credentials; }
    const CredentialsModel & getCredentials() const { return m_credentials; }

    bool CheckAuthenticationState(QString & errorMessage) const;

    const QString GetHostName() const;

signals:
    void statusTextUpdate(QString, const int);
    void showAuthWebPage(QUrl);

public slots:
    void onOAuthSuccess(QString key, QString secret);
    void onOAuthFailure(QString message);
    void onRequestToShowAuthorizationPage(QUrl authUrl);
    void onConsumerKeyAndSecretSet(QString key, QString secret);
    void onUserNameAndPasswordSet(QString name, QString password);

private:
    EvernoteServiceManager(const EvernoteServiceManager & other) = delete;
    EvernoteServiceManager & operator=(const EvernoteServiceManager & other) = delete;

private:
    enum EAuthorizationState
    {
        EAS_AUTHORIZED,
        EAS_UNAUTHORIZED_NEVER_ATTEMPTED,
        EAS_UNAUTHORIZED_CREDENTIALS_REJECTED,
        EAS_UNAUTHORIZED_QUIT,
        EAS_UNAUTHORIZED_INTERNAL_ERROR
    };

    enum EConnectionState
    {
        ECS_CONNECTED,
        ECS_DISCONNECTED
    };

    class EvernoteDataHolder;

    bool CheckConnectionState() const;
    void SetConnectionState(const EConnectionState connectionState);

    void SetDefaultNotebook();
    void SetTrashNotebook();
    void SetFavouriteTag();

private:
    EvernoteServiceOAuthHandler * m_pOAuthHandler;
    EvernoteDataHolder * m_pEvernoteDataHolder;
    CredentialsModel     m_credentials;
    EAuthorizationState  m_authorizationState;
    EConnectionState     m_connectionState;
    QString              m_evernoteHostName;
    double               m_refreshTime;     // Refresh interval in seconds
};

#endif // __QUTE_NOTE__CLIENT__EVERNOTE_SERVICE_MANAGER_H

#ifndef __QUTE_NOTE__EVERNOTE_CLIENT__EN_WRAPPERS_H
#define __QUTE_NOTE__EVERNOTE_CLIENT__EN_WRAPPERS_H

#include <Types_types.h>
#include <NoteStore.h>
#include <QString>
#include <QDataStream>
#include <QByteArray>

namespace qute_note {

QDataStream & operator<<(QDataStream & out, const evernote::edam::BusinessUserInfo & info);
QDataStream & operator>>(QDataStream & in, evernote::edam::BusinessUserInfo & info);

const QByteArray GetSerializedBusinessUserInfo(const evernote::edam::BusinessUserInfo & info);
const evernote::edam::BusinessUserInfo GetDeserializedBusinessUserInfo(const QByteArray & data);

QDataStream & operator<<(QDataStream & out, const evernote::edam::PremiumInfo & info);
QDataStream & operator>>(QDataStream & in, evernote::edam::PremiumInfo & info);

const QByteArray GetSerializedPremiumInfo(const evernote::edam::PremiumInfo & info);
const evernote::edam::PremiumInfo GetDeserializedPremiumInfo(const QByteArray & data);

QDataStream & operator<<(QDataStream & out, const evernote::edam::Accounting & accounting);
QDataStream & operator>>(QDataStream & in, evernote::edam::Accounting & accounting);

const QByteArray GetSerializedAccounting(const evernote::edam::Accounting & accounting);
const evernote::edam::Accounting GetDeserializedAccounting(const QByteArray & data);

QDataStream & operator<<(QDataStream & out, const evernote::edam::UserAttributes & userAttributes);
QDataStream & operator>>(QDataStream & in, evernote::edam::UserAttributes & userAttributes);

const QByteArray GetSerializedUserAttributes(const evernote::edam::UserAttributes & userAttributes);
const evernote::edam::UserAttributes GetDeserializedUserAttributes(const QByteArray & data);

QDataStream & operator<<(QDataStream & out, const evernote::edam::NoteAttributes & noteAttributes);
QDataStream & operator>>(QDataStream & in, evernote::edam::NoteAttributes & noteAttributes);

const QByteArray GetSerializedNoteAttributes(const evernote::edam::NoteAttributes & noteAttributes);
const evernote::edam::NoteAttributes GetDeserializedNoteAttributes(const QByteArray & data);

QDataStream & operator<<(QDataStream & out, const evernote::edam::ResourceAttributes & resourceAttributes);
QDataStream & operator>>(QDataStream & in, evernote::edam::ResourceAttributes & resourceAttributes);

const QByteArray GetSerializedResourceAttributes(const evernote::edam::ResourceAttributes & resourceAttributes);
const evernote::edam::ResourceAttributes GetDeserializedResourceAttributes(const QByteArray & data);

struct Note
{
    Note() : isDirty(true), isLocal(true), isDeleted(false), en_note() {}

    bool isDirty;
    bool isLocal;
    bool isDeleted;
    evernote::edam::Note en_note;

    bool CheckParameters(QString & errorDescription) const;
};

struct Notebook
{
    Notebook() : isDirty(true), isLocal(true), isLastUsed(false), en_notebook() {}

    bool isDirty;
    bool isLocal;
    bool isLastUsed;
    evernote::edam::Notebook en_notebook;

    bool CheckParameters(QString & errorDescription) const;
};

class IResource
{
public:
    IResource();
    IResource(const IResource & other);
    virtual ~IResource();

    bool isDirty() const;
    void SetDirty();
    void SetClean();

    virtual const evernote::edam::Resource & GetEnResource() const = 0;
    virtual evernote::edam::Resource & GetEnResource() = 0;

private:
    IResource & operator=(const IResource & other);
    bool m_isDirty;
};

/**
 * @brief The ResourceWrapper class creates and manages its own evernote::edam::Resource object
 */
class ResourceWrapper : public IResource
{
public:
    ResourceWrapper();
    ResourceWrapper(const ResourceWrapper & other);
    ResourceWrapper & operator=(const ResourceWrapper & other);
    virtual ~ResourceWrapper();

    virtual const evernote::edam::Resource & GetEnResource() const;
    virtual evernote::edam::Resource & GetEnResource();

private:
    evernote::edam::Resource m_en_resource;
};

/**
 * @brief The ResourceAdapter class uses reference to external evernote::edam::Resource
 * and adapts its interface to that of IResource
 */
class ResourceAdapter: public IResource
{
public:
    ResourceAdapter(const evernote::edam::Resource & externalEnResource);
    ResourceAdapter(const ResourceAdapter & other);
    ResourceAdapter & operator=(const ResourceAdapter & other);
    virtual ~ResourceAdapter();

    virtual const evernote::edam::Resource & GetEnResource() const;
    virtual evernote::edam::Resource & GetEnResource();

private:
    evernote::edam::Resource & m_en_resource_ref;
};

struct Resource
{
    Resource() : isDirty(true), en_resource() {}

    bool isDirty;
    evernote::edam::Resource en_resource;

    bool CheckParameters(QString & errorDescription, const bool isFreeAccount = true) const;
    static bool CheckParameters(const evernote::edam::Resource & enResource,
                                QString & errorDescription, const bool isFreeAccount);
};

struct Tag
{
    Tag() : isDirty(true), isLocal(true), isDeleted(false), en_tag() {}

    bool isDirty;
    bool isLocal;
    bool isDeleted;
    evernote::edam::Tag en_tag;

    bool CheckParameters(QString & errorDescription) const;
};

struct SavedSearch
{
    SavedSearch() : isDirty(true), en_search() {}

    bool isDirty;
    evernote::edam::SavedSearch en_search;

    bool CheckParameters(QString & errorDescription) const;
};

struct User
{
    User() : isDirty(true), isLocal(true), en_user() {}

    bool isDirty;
    bool isLocal;
    evernote::edam::User en_user;
};

typedef evernote::edam::Timestamp Timestamp;
typedef evernote::edam::UserID UserID;
typedef evernote::edam::Guid Guid;

bool CheckGuid(const Guid & guid);

bool CheckUpdateSequenceNumber(const int32_t updateSequenceNumber);

}

#endif // __QUTE_NOTE__EVERNOTE_CLIENT__EN_WRAPPERS_H

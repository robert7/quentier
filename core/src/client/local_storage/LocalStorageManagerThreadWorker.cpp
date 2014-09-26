#include "LocalStorageManagerThreadWorker.h"
#include <client/types/UserWrapper.h>
#include <client/types/Notebook.h>
#include <client/types/SharedNotebookWrapper.h>
#include <client/types/LinkedNotebook.h>
#include <client/types/Note.h>
#include <client/types/Tag.h>
#include <client/types/ResourceWrapper.h>
#include <client/types/SavedSearch.h>
#include <logging/QuteNoteLogger.h>

namespace qute_note {

LocalStorageManagerThreadWorker::LocalStorageManagerThreadWorker(const QString & username,
                                                                 const qint32 userId,
                                                                 const bool startFromScratch, QObject * parent) :
    QObject(parent),
    m_localStorageManager(username, userId, startFromScratch),
    m_useCache(true),
    m_localStorageCacheManager()
{}

LocalStorageManagerThreadWorker::~LocalStorageManagerThreadWorker()
{}

void LocalStorageManagerThreadWorker::setUseCache(const bool useCache)
{
    if (m_useCache) {
        // Cache is being disabled - no point to store things in it anymore, it would get rotten pretty quick
        m_localStorageCacheManager.clear();
    }

    m_useCache = useCache;
}

void LocalStorageManagerThreadWorker::onGetUserCountRequest()
{
    QString errorDescription;
    int count = m_localStorageManager.GetUserCount(errorDescription);
    if (count < 0) {
        emit getUserCountFailed(errorDescription);
    }
    else {
        emit getUserCountComplete(count);
    }
}

void LocalStorageManagerThreadWorker::onSwitchUserRequest(QString username, qint32 userId,
                                                          bool startFromScratch)
{
    try {
        m_localStorageManager.SwitchUser(username, userId, startFromScratch);
    }
    catch(const std::exception & exception) {
        emit switchUserFailed(userId, QString(exception.what()));
        return;
    }

    emit switchUserComplete(userId);
}

void LocalStorageManagerThreadWorker::onAddUserRequest(UserWrapper user)
{
    QString errorDescription;

    bool res = m_localStorageManager.AddUser(user, errorDescription);
    if (!res) {
        emit addUserFailed(user, errorDescription);
        return;
    }

    emit addUserComplete(user);
}

void LocalStorageManagerThreadWorker::onUpdateUserRequest(UserWrapper user)
{
    QString errorDescription;

    bool res = m_localStorageManager.UpdateUser(user, errorDescription);
    if (!res) {
        emit updateUserFailed(user, errorDescription);
        return;
    }

    emit updateUserComplete(user);
}

void LocalStorageManagerThreadWorker::onFindUserRequest(UserWrapper user)
{
    QString errorDescription;

    bool res = m_localStorageManager.FindUser(user, errorDescription);
    if (!res) {
        emit findUserFailed(user, errorDescription);
        return;
    }

    emit findUserComplete(user);
}

void LocalStorageManagerThreadWorker::onDeleteUserRequest(UserWrapper user)
{
    QString errorDescription;

    bool res = m_localStorageManager.DeleteUser(user, errorDescription);
    if (!res) {
        emit deleteUserFailed(user, errorDescription);
        return;
    }

    emit deleteUserComplete(user);
}

void LocalStorageManagerThreadWorker::onExpungeUserRequest(UserWrapper user)
{
    QString errorDescription;

    bool res = m_localStorageManager.ExpungeUser(user, errorDescription);
    if (!res) {
        emit expungeUserFailed(user, errorDescription);
        return;
    }

    emit expungeUserComplete(user);
}

void LocalStorageManagerThreadWorker::onGetNotebookCountRequest()
{
    QString errorDescription;
    int count = m_localStorageManager.GetNotebookCount(errorDescription);
    if (count < 0) {
        emit getNotebookCountFailed(errorDescription);
    }
    else {
        emit getNotebookCountComplete(count);
    }
}

void LocalStorageManagerThreadWorker::onAddNotebookRequest(Notebook notebook)
{
    QString errorDescription;

    bool res = m_localStorageManager.AddNotebook(notebook, errorDescription);
    if (!res) {
        emit addNotebookFailed(notebook, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.cacheNotebook(notebook);
    }

    emit addNotebookComplete(notebook);
}

void LocalStorageManagerThreadWorker::onUpdateNotebookRequest(Notebook notebook)
{
    QString errorDescription;

    bool res = m_localStorageManager.UpdateNotebook(notebook, errorDescription);
    if (!res) {
        emit updateNotebookFailed(notebook, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.cacheNotebook(notebook);
    }

    emit updateNotebookComplete(notebook);
}

void LocalStorageManagerThreadWorker::onFindNotebookRequest(Notebook notebook)
{
    QString errorDescription;

    bool foundNotebookInCache = false;
    if (m_useCache)
    {
        bool notebookHasGuid = notebook.hasGuid();
        const QString guid = (notebookHasGuid ? notebook.guid() : notebook.localGuid());
        LocalStorageCacheManager::WhichGuid wg = (notebookHasGuid ? LocalStorageCacheManager::Guid : LocalStorageCacheManager::LocalGuid);

        const Notebook * pNotebook = m_localStorageCacheManager.findNotebook(guid, wg);
        if (pNotebook) {
            notebook = *pNotebook;
            foundNotebookInCache = true;
        }
    }

    if (!foundNotebookInCache)
    {
        bool res = m_localStorageManager.FindNotebook(notebook, errorDescription);
        if (!res) {
            emit findNotebookFailed(notebook, errorDescription);
            return;
        }
    }

    emit findNotebookComplete(notebook);
}

void LocalStorageManagerThreadWorker::onFindDefaultNotebookRequest(Notebook notebook)
{
    QString errorDescription;

    // TODO: employ cache

    bool res = m_localStorageManager.FindDefaultNotebook(notebook, errorDescription);
    if (!res) {
        emit findDefaultNotebookFailed(notebook, errorDescription);
        return;
    }

    emit findDefaultNotebookComplete(notebook);
}

void LocalStorageManagerThreadWorker::onFindLastUsedNotebookRequest(Notebook notebook)
{
    QString errorDescription;

    // TODO: employ cache

    bool res = m_localStorageManager.FindLastUsedNotebook(notebook, errorDescription);
    if (!res) {
        emit findLastUsedNotebookFailed(notebook, errorDescription);
        return;
    }

    emit findLastUsedNotebookComplete(notebook);
}

void LocalStorageManagerThreadWorker::onFindDefaultOrLastUsedNotebookRequest(Notebook notebook)
{
    QString errorDescription;

    // TODO: employ cache

    bool res = m_localStorageManager.FindDefaultOrLastUsedNotebook(notebook, errorDescription);
    if (!res) {
        emit findDefaultOrLastUsedNotebookFailed(notebook, errorDescription);
        return;
    }

    emit findDefaultOrLastUsedNotebookComplete(notebook);
}

void LocalStorageManagerThreadWorker::onListAllNotebooksRequest()
{
    QString errorDescription;
    QList<Notebook> notebooks = m_localStorageManager.ListAllNotebooks(errorDescription);
    if (notebooks.isEmpty() && !errorDescription.isEmpty()) {
        emit listAllNotebooksFailed(errorDescription);
        return;
    }

    if (m_useCache)
    {
        foreach(const Notebook & notebook, notebooks) {
            m_localStorageCacheManager.cacheNotebook(notebook);
        }
    }

    emit listAllNotebooksComplete(notebooks);
}

void LocalStorageManagerThreadWorker::onListAllSharedNotebooksRequest()
{
    QString errorDescription;
    QList<SharedNotebookWrapper> sharedNotebooks = m_localStorageManager.ListAllSharedNotebooks(errorDescription);
    if (sharedNotebooks.isEmpty() && !errorDescription.isEmpty()) {
        emit listAllSharedNotebooksFailed(errorDescription);
        return;
    }

    emit listAllSharedNotebooksComplete(sharedNotebooks);
}

void LocalStorageManagerThreadWorker::onListSharedNotebooksPerNotebookGuidRequest(QString notebookGuid)
{
    QString errorDescription;
    QList<SharedNotebookWrapper> sharedNotebooks = m_localStorageManager.ListSharedNotebooksPerNotebookGuid(notebookGuid, errorDescription);
    if (sharedNotebooks.isEmpty() && !errorDescription.isEmpty()) {
        emit listSharedNotebooksPerNotebookGuidFailed(notebookGuid, errorDescription);
        return;
    }

    emit listSharedNotebooksPerNotebookGuidComplete(notebookGuid, sharedNotebooks);
}

void LocalStorageManagerThreadWorker::onExpungeNotebookRequest(Notebook notebook)
{
    QString errorDescription;

    bool res = m_localStorageManager.ExpungeNotebook(notebook, errorDescription);
    if (!res) {
        emit expungeNotebookFailed(notebook, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.expungeNotebook(notebook);
    }

    emit expungeNotebookComplete(notebook);
}

void LocalStorageManagerThreadWorker::onGetLinkedNotebookCountRequest()
{
    QString errorDescription;
    int count = m_localStorageManager.GetLinkedNotebookCount(errorDescription);
    if (count < 0) {
        emit getLinkedNotebookCountFailed(errorDescription);
    }
    else {
        emit getLinkedNotebookCountComplete(count);
    }
}

void LocalStorageManagerThreadWorker::onAddLinkedNotebookRequest(LinkedNotebook linkedNotebook)
{
    QString errorDescription;

    bool res = m_localStorageManager.AddLinkedNotebook(linkedNotebook, errorDescription);
    if (!res) {
        emit addLinkedNotebookFailed(linkedNotebook, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.cacheLinkedNotebook(linkedNotebook);
    }

    emit addLinkedNotebookComplete(linkedNotebook);
}

void LocalStorageManagerThreadWorker::onUpdateLinkedNotebookRequest(LinkedNotebook linkedNotebook)
{
    QString errorDescription;

    bool res = m_localStorageManager.UpdateLinkedNotebook(linkedNotebook, errorDescription);
    if (!res) {
        emit updateLinkedNotebookFailed(linkedNotebook, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.cacheLinkedNotebook(linkedNotebook);
    }

    emit updateLinkedNotebookComplete(linkedNotebook);
}

void LocalStorageManagerThreadWorker::onFindLinkedNotebookRequest(LinkedNotebook linkedNotebook)
{
    QString errorDescription;

    bool foundLinkedNotebookInCache = false;
    if (m_useCache && linkedNotebook.hasGuid())
    {
        const QString guid = linkedNotebook.guid();
        const LinkedNotebook * pLinkedNotebook = m_localStorageCacheManager.findLinkedNotebook(guid);
        if (pLinkedNotebook) {
            linkedNotebook = *pLinkedNotebook;
            foundLinkedNotebookInCache = true;
        }
    }

    if (!foundLinkedNotebookInCache)
    {
        bool res = m_localStorageManager.FindLinkedNotebook(linkedNotebook, errorDescription);
        if (!res) {
            emit findLinkedNotebookFailed(linkedNotebook, errorDescription);
            return;
        }
    }

    emit findLinkedNotebookComplete(linkedNotebook);
}

void LocalStorageManagerThreadWorker::onListAllLinkedNotebooksRequest()
{
    QString errorDescription;
    QList<LinkedNotebook> linkedNotebooks = m_localStorageManager.ListAllLinkedNotebooks(errorDescription);
    if (linkedNotebooks.isEmpty() && !errorDescription.isEmpty()) {
        emit listAllLinkedNotebooksFailed(errorDescription);
        return;
    }

    if (m_useCache)
    {
        foreach(const LinkedNotebook & linkedNotebook, linkedNotebooks) {
            m_localStorageCacheManager.cacheLinkedNotebook(linkedNotebook);
        }
    }

    emit listAllLinkedNotebooksComplete(linkedNotebooks);
}

void LocalStorageManagerThreadWorker::onExpungeLinkedNotebookRequest(LinkedNotebook linkedNotebook)
{
    QString errorDescription;

    bool res = m_localStorageManager.ExpungeLinkedNotebook(linkedNotebook, errorDescription);
    if (!res) {
        emit expungeLinkedNotebookFailed(linkedNotebook, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.expungeLinkedNotebook(linkedNotebook);
    }

    emit expungeLinkedNotebookComplete(linkedNotebook);
}

void LocalStorageManagerThreadWorker::onGetNoteCountRequest()
{
    QString errorDescription;
    int count = m_localStorageManager.GetNoteCount(errorDescription);
    if (count < 0) {
        emit getNoteCountFailed(errorDescription);
    }
    else {
        emit getNoteCountComplete(count);
    }
}

void LocalStorageManagerThreadWorker::onAddNoteRequest(Note note, Notebook notebook)
{
    QString errorDescription;

    bool res = m_localStorageManager.AddNote(note, notebook, errorDescription);
    if (!res) {
        emit addNoteFailed(note, notebook, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.cacheNote(note);
    }

    emit addNoteComplete(note, notebook);
}

void LocalStorageManagerThreadWorker::onUpdateNoteRequest(Note note, Notebook notebook)
{
    QString errorDescription;

    bool res = m_localStorageManager.UpdateNote(note, notebook, errorDescription);
    if (!res) {
        emit updateNoteFailed(note, notebook, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.cacheNote(note);
    }

    emit updateNoteComplete(note, notebook);
}

void LocalStorageManagerThreadWorker::onFindNoteRequest(Note note, bool withResourceBinaryData)
{
    QString errorDescription;

    bool foundNoteInCache = false;
    if (m_useCache)
    {
        bool noteHasGuid = note.hasGuid();
        const QString guid = (noteHasGuid ? note.guid() : note.localGuid());
        LocalStorageCacheManager::WhichGuid wg = (noteHasGuid ? LocalStorageCacheManager::Guid : LocalStorageCacheManager::LocalGuid);

        const Note * pNote = m_localStorageCacheManager.findNote(guid, wg);
        if (pNote) {
            note = *pNote;
            foundNoteInCache = true;
        }
    }

    if (!foundNoteInCache)
    {
        bool res = m_localStorageManager.FindNote(note, errorDescription, withResourceBinaryData);
        if (!res) {
            emit findNoteFailed(note, withResourceBinaryData, errorDescription);
            return;
        }
    }

    emit findNoteComplete(note, withResourceBinaryData);
}

void LocalStorageManagerThreadWorker::onListAllNotesPerNotebookRequest(Notebook notebook,
                                                                       bool withResourceBinaryData)
{
    QString errorDescription;

    QList<Note> notes = m_localStorageManager.ListAllNotesPerNotebook(notebook, errorDescription,
                                                                      withResourceBinaryData);
    if (notes.isEmpty() && !errorDescription.isEmpty()) {
        emit listAllNotesPerNotebookFailed(notebook, withResourceBinaryData, errorDescription);
        return;
    }

    if (m_useCache)
    {
        foreach(const Note & note, notes) {
            m_localStorageCacheManager.cacheNote(note);
        }
    }

    emit listAllNotesPerNotebookComplete(notebook, withResourceBinaryData, notes);
}

void LocalStorageManagerThreadWorker::onDeleteNoteRequest(Note note)
{
    QString errorDescription;

    bool res = m_localStorageManager.DeleteNote(note, errorDescription);
    if (!res) {
        emit deleteNoteFailed(note, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.cacheNote(note);
    }

    emit deleteNoteComplete(note);
}

void LocalStorageManagerThreadWorker::onExpungeNoteRequest(Note note)
{
    QString errorDescription;

    bool res = m_localStorageManager.ExpungeNote(note, errorDescription);
    if (!res) {
        emit expungeNoteFailed(note, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.expungeNote(note);
    }

    emit expungeNoteComplete(note);
}

void LocalStorageManagerThreadWorker::onGetTagCountRequest()
{
    QString errorDescription;
    int count = m_localStorageManager.GetTagCount(errorDescription);
    if (count < 0) {
        emit getTagCountFailed(errorDescription);
    }
    else {
        emit getTagCountComplete(count);
    }
}

void LocalStorageManagerThreadWorker::onAddTagRequest(Tag tag)
{
    QString errorDescription;

    bool res = m_localStorageManager.AddTag(tag, errorDescription);
    if (!res) {
        emit addTagFailed(tag, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.cacheTag(tag);
    }

    emit addTagComplete(tag);
}

void LocalStorageManagerThreadWorker::onUpdateTagRequest(Tag tag)
{
    QString errorDescription;

    bool res = m_localStorageManager.UpdateTag(tag, errorDescription);
    if (!res) {
        emit updateTagFailed(tag, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.cacheTag(tag);
    }

    emit updateTagComplete(tag);
}

void LocalStorageManagerThreadWorker::onLinkTagWithNoteRequest(Tag tag, Note note)
{
    QString errorDescription;

    bool res = m_localStorageManager.LinkTagWithNote(tag, note, errorDescription);
    if (!res) {
        emit linkTagWithNoteFailed(tag, note, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.cacheTag(tag);
    }

    emit linkTagWithNoteComplete(tag, note);
}

void LocalStorageManagerThreadWorker::onFindTagRequest(Tag tag)
{
    QString errorDescription;

    bool foundTagInCache = false;
    if (m_useCache)
    {
        bool tagHasGuid = tag.hasGuid();
        const QString guid = (tagHasGuid ? tag.guid() : tag.localGuid());
        LocalStorageCacheManager::WhichGuid wg = (tagHasGuid ? LocalStorageCacheManager::Guid : LocalStorageCacheManager::LocalGuid);

        const Tag * pTag = m_localStorageCacheManager.findTag(guid, wg);
        if (pTag) {
            tag = *pTag;
            foundTagInCache = true;
        }
    }

    if (!foundTagInCache)
    {
        bool res = m_localStorageManager.FindTag(tag, errorDescription);
        if (!res) {
            emit findTagFailed(tag, errorDescription);
            return;
        }
    }

    emit findTagComplete(tag);
}

void LocalStorageManagerThreadWorker::onListAllTagsPerNoteRequest(Note note)
{
    QString errorDescription;

    QList<Tag> tags = m_localStorageManager.ListAllTagsPerNote(note, errorDescription);
    if (tags.isEmpty() && !errorDescription.isEmpty()) {
        emit listAllTagsPerNoteFailed(note, errorDescription);
        return;
    }

    if (m_useCache)
    {
        foreach(const Tag & tag, tags) {
            m_localStorageCacheManager.cacheTag(tag);
        }
    }

    emit listAllTagsPerNoteComplete(tags, note);
}

void LocalStorageManagerThreadWorker::onListAllTagsRequest()
{
    QString errorDescription;

    QList<Tag> tags = m_localStorageManager.ListAllTags(errorDescription);
    if (tags.isEmpty() && !errorDescription.isEmpty()) {
        emit listAllTagsFailed(errorDescription);
        return;
    }

    if (m_useCache)
    {
        foreach(const Tag & tag, tags) {
            m_localStorageCacheManager.cacheTag(tag);
        }
    }

    emit listAllTagsComplete(tags);
}

void LocalStorageManagerThreadWorker::onDeleteTagRequest(Tag tag)
{
    QString errorDescription;

    bool res = m_localStorageManager.DeleteTag(tag, errorDescription);
    if (!res) {
        emit deleteTagFailed(tag, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.cacheTag(tag);
    }

    emit deleteTagComplete(tag);
}

void LocalStorageManagerThreadWorker::onExpungeTagRequest(Tag tag)
{
    QString errorDescription;

    bool res = m_localStorageManager.ExpungeTag(tag, errorDescription);
    if (!res) {
        emit expungeTagFailed(tag, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.expungeTag(tag);
    }

    emit expungeTagComplete(tag);
}

void LocalStorageManagerThreadWorker::onGetResourceCountRequest()
{
    QString errorDescription;
    int count = m_localStorageManager.GetEnResourceCount(errorDescription);
    if (count < 0) {
        emit getResourceCountFailed(errorDescription);
    }
    else {
        emit getResourceCountComplete(count);
    }
}

void LocalStorageManagerThreadWorker::onAddResourceRequest(ResourceWrapper resource, Note note)
{
    QString errorDescription;

    bool res = m_localStorageManager.AddEnResource(resource, note, errorDescription);
    if (!res) {
        emit addResourceFailed(resource, note, errorDescription);
        return;
    }

    emit addResourceComplete(resource, note);
}

void LocalStorageManagerThreadWorker::onUpdateResourceRequest(ResourceWrapper resource, Note note)
{
    QString errorDescription;

    bool res = m_localStorageManager.UpdateEnResource(resource, note, errorDescription);
    if (!res) {
        emit updateResourceFailed(resource, note, errorDescription);
        return;
    }

    emit updateResourceComplete(resource, note);
}

void LocalStorageManagerThreadWorker::onFindResourceRequest(ResourceWrapper resource, bool withBinaryData)
{
    QString errorDescription;

    bool res = m_localStorageManager.FindEnResource(resource, errorDescription, withBinaryData);
    if (!res) {
        emit findResourceFailed(resource, withBinaryData, errorDescription);
        return;
    }

    emit findResourceComplete(resource, withBinaryData);
}

void LocalStorageManagerThreadWorker::onExpungeResourceRequest(ResourceWrapper resource)
{
    QString errorDescription;

    bool res = m_localStorageManager.ExpungeEnResource(resource, errorDescription);
    if (!res) {
        emit expungeResourceFailed(resource, errorDescription);
        return;
    }

    emit expungeResourceComplete(resource);
}

void LocalStorageManagerThreadWorker::onGetSavedSearchCountRequest()
{
    QString errorDescription;
    int count = m_localStorageManager.GetSavedSearchCount(errorDescription);
    if (count < 0) {
        emit getSavedSearchCountFailed(errorDescription);
    }
    else {
        emit getSavedSearchCountComplete(count);
    }
}

void LocalStorageManagerThreadWorker::onAddSavedSearchRequest(SavedSearch search)
{
    QString errorDescription;

    bool res = m_localStorageManager.AddSavedSearch(search, errorDescription);
    if (!res) {
        emit addSavedSearchFailed(search, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.cacheSavedSearch(search);
    }

    emit addSavedSearchComplete(search);
}

void LocalStorageManagerThreadWorker::onUpdateSavedSearchRequest(SavedSearch search)
{
    QString errorDescription;

    bool res = m_localStorageManager.UpdateSavedSearch(search, errorDescription);
    if (!res) {
        emit updateSavedSearchFailed(search, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.cacheSavedSearch(search);
    }

    emit updateSavedSearchComplete(search);
}

void LocalStorageManagerThreadWorker::onFindSavedSearchRequest(SavedSearch search)
{
    QString errorDescription;

    bool foundCachedSavedSearch = false;
    if (m_useCache)
    {
        bool searchHasGuid = search.hasGuid();
        const QString guid = (searchHasGuid ? search.guid() : search.localGuid());
        const LocalStorageCacheManager::WhichGuid wg = (searchHasGuid ? LocalStorageCacheManager::Guid : LocalStorageCacheManager::LocalGuid);

        const SavedSearch * pSearch = m_localStorageCacheManager.findSavedSearch(guid, wg);
        if (pSearch) {
            search = *pSearch;
            foundCachedSavedSearch = true;
        }
    }

    if (!foundCachedSavedSearch)
    {
        bool res = m_localStorageManager.FindSavedSearch(search, errorDescription);
        if (!res) {
            emit findSavedSearchFailed(search, errorDescription);
            return;
        }
    }

    emit findSavedSearchComplete(search);
}

void LocalStorageManagerThreadWorker::onListAllSavedSearchesRequest()
{
    QString errorDescription;
    QList<SavedSearch> searches = m_localStorageManager.ListAllSavedSearches(errorDescription);
    if (searches.isEmpty() && !errorDescription.isEmpty()) {
        emit listAllSavedSearchesFailed(errorDescription);
        return;
    }

    if (m_useCache)
    {
        foreach(const SavedSearch & search, searches) {
            m_localStorageCacheManager.cacheSavedSearch(search);
        }
    }

    emit listAllSavedSearchesComplete(searches);
}

void LocalStorageManagerThreadWorker::onExpungeSavedSearch(SavedSearch search)
{
    QString errorDescription;

    bool res = m_localStorageManager.ExpungeSavedSearch(search, errorDescription);
    if (!res) {
        emit expungeSavedSearchFailed(search, errorDescription);
        return;
    }

    if (m_useCache) {
        m_localStorageCacheManager.expungeSavedSearch(search);
    }

    emit expungeSavedSearchComplete(search);
}

} // namespace qute_note

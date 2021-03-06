/*
 * Copyright 2016 Dmitry Ivanov
 *
 * This file is part of Quentier.
 *
 * Quentier is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * Quentier is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Quentier. If not, see <http://www.gnu.org/licenses/>.
 */

#include "FavoritesModel.h"
#include "NoteModel.h"
#include <quentier/logging/QuentierLogger.h>

// Limit for the queries to the local storage
#define NOTE_LIST_LIMIT (40)
#define NOTEBOOK_LIST_LIMIT (40)
#define TAG_LIST_LIMIT (40)
#define SAVED_SEARCH_LIST_LIMIT (40)

#define NUM_FAVORITES_MODEL_COLUMNS (3)

namespace quentier {

FavoritesModel::FavoritesModel(const Account & account, const NoteModel & noteModel,
                               LocalStorageManagerAsync & localStorageManagerAsync,
                               NoteCache & noteCache, NotebookCache & notebookCache, TagCache & tagCache,
                               SavedSearchCache & savedSearchCache, QObject * parent) :
    QAbstractItemModel(parent),
    m_account(account),
    m_data(),
    m_noteCache(noteCache),
    m_notebookCache(notebookCache),
    m_tagCache(tagCache),
    m_savedSearchCache(savedSearchCache),
    m_lowerCaseNotebookNames(),
    m_lowerCaseTagNames(),
    m_lowerCaseSavedSearchNames(),
    m_listNotesOffset(0),
    m_listNotesRequestId(),
    m_listNotebooksOffset(0),
    m_listNotebooksRequestId(),
    m_listTagsOffset(0),
    m_listTagsRequestId(),
    m_listSavedSearchesOffset(0),
    m_listSavedSearchesRequestId(),
    m_updateNoteRequestIds(),
    m_findNoteToRestoreFailedUpdateRequestIds(),
    m_findNoteToPerformUpdateRequestIds(),
    m_findNoteToUnfavoriteRequestIds(),
    m_updateNotebookRequestIds(),
    m_findNotebookToRestoreFailedUpdateRequestIds(),
    m_findNotebookToPerformUpdateRequestIds(),
    m_findNotebookToUnfavoriteRequestIds(),
    m_updateTagRequestIds(),
    m_findTagToRestoreFailedUpdateRequestIds(),
    m_findTagToPerformUpdateRequestIds(),
    m_findTagToUnfavoriteRequestIds(),
    m_updateSavedSearchRequestIds(),
    m_findSavedSearchToRestoreFailedUpdateRequestIds(),
    m_findSavedSearchToPerformUpdateRequestIds(),
    m_findSavedSearchToUnfavoriteRequestIds(),
    m_tagLocalUidToLinkedNotebookGuid(),
    m_notebookLocalUidToGuid(),
    m_notebookLocalUidByNoteLocalUid(),
    m_receivedNotebookLocalUidsForAllNotes(false),
    m_tagLocalUidsByNoteLocalUid(),
    m_receivedTagLocalUidsForAllNotes(false),
    m_notebookLocalUidToNoteCountRequestIdBimap(),
    m_tagLocalUidToNoteCountRequestIdBimap(),
    m_sortedColumn(Columns::DisplayName),
    m_sortOrder(Qt::AscendingOrder),
    m_allItemsListed(false)
{
    createConnections(noteModel, localStorageManagerAsync);

    if (noteModel.allNotesListed()) {
        buildTagLocalUidsByNoteLocalUidsHash(noteModel);
        buildNotebookLocalUidByNoteLocalUidsHash(noteModel);
    }

    requestNotebooksList();
    requestTagsList();
    requestNotesList();
    requestSavedSearchesList();
}

FavoritesModel::~FavoritesModel()
{}

void FavoritesModel::updateAccount(const Account & account)
{
    QNDEBUG(QStringLiteral("FavoritesModel::updateAccount: ") << account);
    m_account = account;
}

QModelIndex FavoritesModel::indexForLocalUid(const QString & localUid) const
{
    const FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
    auto it = localUidIndex.find(localUid);
    if (Q_UNLIKELY(it == localUidIndex.end())) {
        QNDEBUG(QStringLiteral("Can't find favorites model item by local uid: ") << localUid);
        return QModelIndex();
    }

    const FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();
    auto indexIt = m_data.project<ByIndex>(it);
    if (Q_UNLIKELY(indexIt == rowIndex.end())) {
        QNWARNING(QStringLiteral("Can't find the indexed reference to the favorites model item: ") << *it);
        return QModelIndex();
    }

    int row = static_cast<int>(std::distance(rowIndex.begin(), indexIt));
    return createIndex(row, Columns::DisplayName);
}

const FavoritesModelItem * FavoritesModel::itemForLocalUid(const QString & localUid) const
{
    const FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
    auto it = localUidIndex.find(localUid);
    if (Q_UNLIKELY(it == localUidIndex.end())) {
        QNDEBUG(QStringLiteral("Can't find favorites model item by local uid: ") << localUid);
        return Q_NULLPTR;
    }

    return &(*it);
}

const FavoritesModelItem * FavoritesModel::itemAtRow(const int row) const
{
    const FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();
    if (Q_UNLIKELY((row < 0) || (rowIndex.size() <= static_cast<size_t>(row)))) {
        QNDEBUG(QStringLiteral("Detected attempt to get the favorites model item for non-existing row"));
        return Q_NULLPTR;
    }

    return &(rowIndex[static_cast<size_t>(row)]);
}

Qt::ItemFlags FavoritesModel::flags(const QModelIndex & index) const
{
    Qt::ItemFlags indexFlags = QAbstractItemModel::flags(index);
    if (!index.isValid()) {
        return indexFlags;
    }

    if ((index.row() < 0) || (index.row() >= static_cast<int>(m_data.size())) ||
        (index.column() < 0) || (index.column() >= NUM_FAVORITES_MODEL_COLUMNS))
    {
        return indexFlags;
    }

    indexFlags |= Qt::ItemIsSelectable;
    indexFlags |= Qt::ItemIsEnabled;

    const FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();
    const FavoritesModelItem & item = rowIndex.at(static_cast<size_t>(index.row()));

    switch(item.type())
    {
    case FavoritesModelItem::Type::Notebook:
        if (canUpdateNotebook(item.localUid())) {
            indexFlags |= Qt::ItemIsEditable;
        }
        break;
    case FavoritesModelItem::Type::Note:
        if (canUpdateNote(item.localUid())) {
            indexFlags |= Qt::ItemIsEditable;
        }
        break;
    case FavoritesModelItem::Type::Tag:
        if (canUpdateTag(item.localUid())) {
            indexFlags |= Qt::ItemIsEditable;
        }
        break;
    default:
        indexFlags |= Qt::ItemIsEditable;
        break;
    }

    return indexFlags;
}

QVariant FavoritesModel::data(const QModelIndex & index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }

    if ((index.row() < 0) || (index.row() >= static_cast<int>(m_data.size())) ||
        (index.column() < 0) || (index.column() >= NUM_FAVORITES_MODEL_COLUMNS))
    {
        return QVariant();
    }

    Columns::type column;
    switch(index.column())
    {
    case Columns::Type:
    case Columns::DisplayName:
    case Columns::NumNotesTargeted:
        column = static_cast<Columns::type>(index.column());
        break;
    default:
        return QVariant();
    }

    switch(role)
    {
    case Qt::DisplayRole:
    case Qt::EditRole:
    case Qt::ToolTipRole:
        return dataImpl(index.row(), column);
    case Qt::AccessibleTextRole:
    case Qt::AccessibleDescriptionRole:
        return dataAccessibleText(index.row(), column);
    default:
        return QVariant();
    }
}

QVariant FavoritesModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole) {
        return QVariant();
    }

    if (orientation == Qt::Vertical) {
        return QVariant(section + 1);
    }

    switch(section)
    {
    case Columns::Type:
        // TRANSLATOR: the type of the favorites model item - note, notebook, tag or saved search
        return QVariant(tr("Type"));
    case Columns::DisplayName:
        // TRANSLATOR: the displayed name of the favorites model item - the name of a notebook or tag or saved search of the note's title
        return QVariant(tr("Name"));
    case Columns::NumNotesTargeted:
        // TRANSLATOR: the number of items targeted by the favorites model item, in particular, the number of notes targeted by the notebook or tag
        return QVariant(tr("N items"));
    default:
        return QVariant();
    }
}

int FavoritesModel::rowCount(const QModelIndex & parent) const
{
    if (parent.isValid()) {
        return 0;
    }

    return static_cast<int>(m_data.size());
}

int FavoritesModel::columnCount(const QModelIndex & parent) const
{
    if (parent.isValid()) {
        return 0;
    }

    return NUM_FAVORITES_MODEL_COLUMNS;
}

QModelIndex FavoritesModel::index(int row, int column, const QModelIndex & parent) const
{
    if (parent.isValid()) {
        return QModelIndex();
    }

    if ((row < 0) || (row >= static_cast<int>(m_data.size())) ||
        (column < 0) || (column >= NUM_FAVORITES_MODEL_COLUMNS))
    {
        return QModelIndex();
    }

    return createIndex(row, column);
}

QModelIndex FavoritesModel::parent(const QModelIndex & index) const
{
    Q_UNUSED(index)
    return QModelIndex();
}

bool FavoritesModel::setHeaderData(int section, Qt::Orientation orientation, const QVariant & value, int role)
{
    Q_UNUSED(section)
    Q_UNUSED(orientation)
    Q_UNUSED(value)
    Q_UNUSED(role)
    return false;
}

bool FavoritesModel::setData(const QModelIndex & index, const QVariant & value, int role)
{
    if (!index.isValid()) {
        return false;
    }

    if (role != Qt::EditRole) {
        return false;
    }

    if ((index.row() < 0) || (index.row() >= static_cast<int>(m_data.size())) ||
        (index.column() < 0) || (index.column() >= NUM_FAVORITES_MODEL_COLUMNS))
    {
        return false;
    }

    FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();
    FavoritesModelItem item = rowIndex.at(static_cast<size_t>(index.row()));

    switch(item.type())
    {
    case FavoritesModelItem::Type::Notebook:
        if (!canUpdateNotebook(item.localUid())) {
            return false;
        }
        break;
    case FavoritesModelItem::Type::Note:
        if (!canUpdateNote(item.localUid())) {
            return false;
        }
        break;
    case FavoritesModelItem::Type::Tag:
        if (!canUpdateTag(item.localUid())) {
            return false;
        }
        break;
    default:
        break;
    }

    switch(index.column())
    {
    case Columns::DisplayName:
        {
            QString newDisplayName = value.toString().trimmed();
            bool changed = (item.displayName() != newDisplayName);
            if (!changed) {
                return true;
            }

            switch(item.type())
            {
            case FavoritesModelItem::Type::Notebook:
                {
                    auto it = m_lowerCaseNotebookNames.find(newDisplayName.toLower());
                    if (it != m_lowerCaseNotebookNames.end()) {
                        ErrorString error(QT_TR_NOOP("Can't rename the notebook: no two notebooks within the account "
                                                     "are allowed to have the same name in case-insensitive manner"));
                        error.details() = newDisplayName;
                        QNINFO(error);
                        Q_EMIT notifyError(error);
                        return false;
                    }

                    ErrorString errorDescription;
                    if (!Notebook::validateName(newDisplayName, &errorDescription)) {
                        ErrorString error(QT_TR_NOOP("Can't rename the notebook"));
                        error.appendBase(errorDescription.base());
                        error.appendBase(errorDescription.additionalBases());
                        error.details() = errorDescription.details();
                        QNINFO(error << QStringLiteral(", suggested new name = ") << newDisplayName);
                        Q_EMIT notifyError(error);
                        return false;
                    }

                    // Removing the previous name and inserting the new one
                    it = m_lowerCaseNotebookNames.find(item.displayName().toLower());
                    if (Q_LIKELY(it != m_lowerCaseNotebookNames.end())) {
                        Q_UNUSED(m_lowerCaseNotebookNames.erase(it))
                    }

                    Q_UNUSED(m_lowerCaseNotebookNames.insert(newDisplayName.toLower()))

                    break;
                }
            case FavoritesModelItem::Type::Tag:
                {
                    auto it = m_lowerCaseTagNames.find(newDisplayName.toLower());
                    if (it != m_lowerCaseTagNames.end()) {
                        ErrorString error(QT_TR_NOOP("Can't rename the tag: no two tags within the account are allowed "
                                                     "to have the same name in case-insensitive manner"));
                        QNINFO(error);
                        Q_EMIT notifyError(error);
                        return false;
                    }

                    ErrorString errorDescription;
                    if (!Tag::validateName(newDisplayName, &errorDescription)) {
                        ErrorString error(QT_TR_NOOP("Can't rename the tag"));
                        error.appendBase(errorDescription.base());
                        error.appendBase(errorDescription.additionalBases());
                        error.details() = errorDescription.details();
                        QNINFO(error << QStringLiteral(", suggested new name = ") << newDisplayName);
                        Q_EMIT notifyError(error);
                        return false;
                    }

                    // Removing the previous name and inserting the new one
                    it = m_lowerCaseTagNames.find(item.displayName().toLower());
                    if (Q_LIKELY(it != m_lowerCaseTagNames.end())) {
                        Q_UNUSED(m_lowerCaseTagNames.erase(it))
                    }

                    Q_UNUSED(m_lowerCaseTagNames.insert(newDisplayName.toLower()))

                    break;
                }
            case FavoritesModelItem::Type::SavedSearch:
                {
                    auto it = m_lowerCaseSavedSearchNames.find(newDisplayName.toLower());
                    if (it != m_lowerCaseSavedSearchNames.end()) {
                        ErrorString error(QT_TR_NOOP("Can't rename the saved search: no two saved searches within the account "
                                                     "are allowed to have the same name in case-insensitive manner"));
                        QNINFO(error);
                        Q_EMIT notifyError(error);
                        return false;
                    }

                    ErrorString errorDescription;
                    if (!SavedSearch::validateName(newDisplayName, &errorDescription)) {
                        ErrorString error(QT_TR_NOOP("Can't rename the saved search"));
                        error.appendBase(errorDescription.base());
                        error.appendBase(errorDescription.additionalBases());
                        error.details() = errorDescription.details();
                        QNINFO(error << QStringLiteral(", suggested new name = ") << newDisplayName);
                        Q_EMIT notifyError(error);
                        return false;
                    }

                    // Removing the previous name and inserting the new one
                    it = m_lowerCaseSavedSearchNames.find(item.displayName().toLower());
                    if (Q_LIKELY(it != m_lowerCaseSavedSearchNames.end())) {
                        Q_UNUSED(m_lowerCaseSavedSearchNames.erase(it))
                    }

                    Q_UNUSED(m_lowerCaseSavedSearchNames.insert(newDisplayName.toLower()))

                    break;
                }
            default:
                break;
            }

            item.setDisplayName(newDisplayName);
            break;
        }
    default:
        return false;
    }

    rowIndex.replace(rowIndex.begin() + index.row(), item);
    Q_EMIT dataChanged(index, index);

    updateItemRowWithRespectToSorting(item);

    updateItemInLocalStorage(item);
    return true;
}

bool FavoritesModel::insertRows(int row, int count, const QModelIndex & parent)
{
    // Not implemented for this model
    Q_UNUSED(row)
    Q_UNUSED(count)
    Q_UNUSED(parent)
    return false;
}

bool FavoritesModel::removeRows(int row, int count, const QModelIndex & parent)
{
    if (Q_UNLIKELY(parent.isValid())) {
        QNDEBUG(QStringLiteral("Ignoring the attempt to remove rows from favorites model for valid parent model index"));
        return false;
    }

    if (Q_UNLIKELY((row + count - 1) >= static_cast<int>(m_data.size()))) {
        ErrorString error(QT_TR_NOOP("Detected attempt to remove more rows than the favorites model contains"));
        QNINFO(error << QStringLiteral(", row = ") << row << QStringLiteral(", count = ") << count
               << QStringLiteral(", number of favorites model items = ") << m_data.size());
        Q_EMIT notifyError(error);
        return false;
    }

    FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();

    beginRemoveRows(QModelIndex(), row, row + count - 1);

    QStringList notebookLocalUidsToRemove;
    QStringList noteLocalUidsToRemove;
    QStringList tagLocalUidsToRemove;
    QStringList savedSearchLocalUidsToRemove;

    // NOTE: just a wild guess about how many items of each kind we can meet
    notebookLocalUidsToRemove.reserve(count / 4);
    noteLocalUidsToRemove.reserve(count / 4);
    tagLocalUidsToRemove.reserve(count / 4);
    savedSearchLocalUidsToRemove.reserve(count / 4);

    for(int i = 0; i < count; ++i)
    {
        auto it = rowIndex.begin() + row + i;
        const FavoritesModelItem & item = *it;

        switch(item.type())
        {
        case FavoritesModelItem::Type::Notebook:
            notebookLocalUidsToRemove << item.localUid();
            break;
        case FavoritesModelItem::Type::Note:
            noteLocalUidsToRemove << item.localUid();
            break;
        case FavoritesModelItem::Type::Tag:
            tagLocalUidsToRemove << item.localUid();
            break;
        case FavoritesModelItem::Type::SavedSearch:
            savedSearchLocalUidsToRemove << item.localUid();
            break;
        default:
            {
                QNDEBUG(QStringLiteral("Favorites model item with unknown type detected: ") << item);
                break;
            }
        }
    }

    Q_UNUSED(rowIndex.erase(rowIndex.begin() + row, rowIndex.begin() + row + count))

    for(auto it = notebookLocalUidsToRemove.begin(), end = notebookLocalUidsToRemove.end(); it != end; ++it) {
        unfavoriteNotebook(*it);
    }

    for(auto it = noteLocalUidsToRemove.begin(), end = noteLocalUidsToRemove.end(); it != end; ++it) {
        unfavoriteNote(*it);
    }

    for(auto it = tagLocalUidsToRemove.begin(), end = tagLocalUidsToRemove.end(); it != end; ++it) {
        unfavoriteTag(*it);
    }

    for(auto it = savedSearchLocalUidsToRemove.begin(), end = savedSearchLocalUidsToRemove.end(); it != end; ++it) {
        unfavoriteSavedSearch(*it);
    }

    endRemoveRows();

    return true;
}

void FavoritesModel::sort(int column, Qt::SortOrder order)
{
    QNDEBUG(QStringLiteral("FavoritesModel::sort: column = ") << column << QStringLiteral(", order = ") << order
            << QStringLiteral(" (") << (order == Qt::AscendingOrder ? QStringLiteral("ascending") : QStringLiteral("descending"))
            << QStringLiteral(")"));

    if (Q_UNLIKELY((column < 0) || (column >= NUM_FAVORITES_MODEL_COLUMNS))) {
        return;
    }

    FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();

    if (column == m_sortedColumn)
    {
        if (order == m_sortOrder) {
            QNDEBUG(QStringLiteral("Neither sorted column nor sort order have changed, nothing to do"));
            return;
        }

        m_sortOrder = order;

        QNDEBUG(QStringLiteral("Only the sort order has changed, reversing the index"));

        Q_EMIT layoutAboutToBeChanged();
        rowIndex.reverse();
        Q_EMIT layoutChanged();

        return;
    }

    m_sortedColumn = static_cast<Columns::type>(column);
    m_sortOrder = order;

    Q_EMIT layoutAboutToBeChanged();

    QModelIndexList persistentIndices = persistentIndexList();
    QVector<std::pair<QString, int> > localUidsToUpdateWithColumns;
    localUidsToUpdateWithColumns.reserve(persistentIndices.size());

    QStringList localUidsToUpdate;
    for(auto it = persistentIndices.begin(), end = persistentIndices.end(); it != end; ++it)
    {
        const QModelIndex & modelIndex = *it;
        int column = modelIndex.column();

        if (!modelIndex.isValid()) {
            localUidsToUpdateWithColumns << std::pair<QString, int>(QString(), column);
            continue;
        }

        int row = modelIndex.row();

        if ((row < 0) || (row >= static_cast<int>(m_data.size())) ||
            (column < 0) || (column >= NUM_FAVORITES_MODEL_COLUMNS))
        {
            localUidsToUpdateWithColumns << std::pair<QString, int>(QString(), column);
        }

        auto itemIt = rowIndex.begin() + row;
        const FavoritesModelItem & item = *itemIt;
        localUidsToUpdateWithColumns << std::pair<QString, int>(item.localUid(), column);
    }

    std::vector<boost::reference_wrapper<const FavoritesModelItem> > items(rowIndex.begin(), rowIndex.end());
    std::sort(items.begin(), items.end(), Comparator(m_sortedColumn, m_sortOrder));
    rowIndex.rearrange(items.begin());

    QModelIndexList replacementIndices;
    replacementIndices.reserve(std::max(localUidsToUpdateWithColumns.size(), 0));
    for(auto it = localUidsToUpdateWithColumns.begin(), end = localUidsToUpdateWithColumns.end(); it != end; ++it)
    {
        const QString & localUid = it->first;
        const int column = it->second;

        if (localUid.isEmpty()) {
            replacementIndices << QModelIndex();
            continue;
        }

        QModelIndex newIndex = indexForLocalUid(localUid);
        if (!newIndex.isValid()) {
            replacementIndices << QModelIndex();
            continue;
        }

        QModelIndex newIndexWithColumn = createIndex(newIndex.row(), column);
        replacementIndices << newIndexWithColumn;
    }

    changePersistentIndexList(persistentIndices, replacementIndices);

    Q_EMIT layoutChanged();
}

void FavoritesModel::onAllNotesListed()
{
    QNDEBUG(QStringLiteral("FavoritesModel::onAllNotesListed"));

    NoteModel * pNoteModel = qobject_cast<NoteModel*>(sender());
    if (Q_UNLIKELY(!pNoteModel)) {
        ErrorString errorDescription(QT_TR_NOOP("Internal error: received all notes listed event from some sender "
                                                "which is not an instance of NoteModel"));
        QNWARNING(errorDescription);
        Q_EMIT notifyError(errorDescription);
        return;
    }

    QObject::disconnect(pNoteModel, QNSIGNAL(NoteModel,notifyAllNotesListed),
                        this, QNSLOT(FavoritesModel,onAllNotesListed));

    buildTagLocalUidsByNoteLocalUidsHash(*pNoteModel);
    buildNotebookLocalUidByNoteLocalUidsHash(*pNoteModel);
}

void FavoritesModel::onAddNoteComplete(Note note, QUuid requestId)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onAddNoteComplete: note = ") << note << QStringLiteral("\nRequest id = ") << requestId);
    onNoteAddedOrUpdated(note);
}

void FavoritesModel::onUpdateNoteComplete(Note note, bool updateResources, bool updateTags, QUuid requestId)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onUpdateNoteComplete: note = ") << note << QStringLiteral("\nUpdate resources = ")
            << (updateResources ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", update tags = ")
            << (updateTags ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", request id = ") << requestId);

    auto it = m_updateNoteRequestIds.find(requestId);
    if (it != m_updateNoteRequestIds.end()) {
        Q_UNUSED(m_updateNoteRequestIds.erase(it))
        return;
    }

    onNoteAddedOrUpdated(note, updateTags);
}

void FavoritesModel::onUpdateNoteFailed(Note note, bool updateResources, bool updateTags,
                                        ErrorString errorDescription, QUuid requestId)
{
    auto it = m_updateNoteRequestIds.find(requestId);
    if (it == m_updateNoteRequestIds.end()) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onUpdateNoteFailed: note = ") << note << QStringLiteral("\nUpdate resources = ")
            << (updateResources ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", update tags = ")
            << (updateTags ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", error descrition: ")
            << errorDescription << QStringLiteral(", request id = ") << requestId);

    Q_UNUSED(m_updateNoteRequestIds.erase(it))

    requestId = QUuid::createUuid();
    Q_UNUSED(m_findNoteToRestoreFailedUpdateRequestIds.insert(requestId))
    QNTRACE(QStringLiteral("Emitting the request to find a note: local uid = ") << note.localUid()
            << QStringLiteral(", request id = ") << requestId);
    Q_EMIT findNote(note, /* with resource metadata = */ true, /* with resource binary data = */ false, requestId);
}

void FavoritesModel::onFindNoteComplete(Note note, bool withResourceMetadata, bool withResourceBinaryData, QUuid requestId)
{
    auto restoreUpdateIt = m_findNoteToRestoreFailedUpdateRequestIds.find(requestId);
    auto performUpdateIt = m_findNoteToPerformUpdateRequestIds.find(requestId);
    auto unfavoriteIt = m_findNoteToUnfavoriteRequestIds.find(requestId);

    if ((restoreUpdateIt == m_findNoteToRestoreFailedUpdateRequestIds.end()) &&
        (performUpdateIt == m_findNoteToPerformUpdateRequestIds.end()) &&
        (unfavoriteIt == m_findNoteToUnfavoriteRequestIds.end()))
    {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onFindNoteComplete: note = ") << note << QStringLiteral(", with resource metadata = ")
            << (withResourceMetadata ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", with resource binary data = ")
            << (withResourceBinaryData ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", request id = ") << requestId);

    if (restoreUpdateIt != m_findNoteToRestoreFailedUpdateRequestIds.end())
    {
        Q_UNUSED(m_findNoteToRestoreFailedUpdateRequestIds.erase(restoreUpdateIt))
        onNoteAddedOrUpdated(note);
    }
    else if (performUpdateIt != m_findNoteToPerformUpdateRequestIds.end())
    {
        Q_UNUSED(m_findNoteToPerformUpdateRequestIds.erase(performUpdateIt))
        m_noteCache.put(note.localUid(), note);

        FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
        auto it = localUidIndex.find(note.localUid());
        if (it != localUidIndex.end()) {
            updateItemInLocalStorage(*it);
        }
    }
    else if (unfavoriteIt != m_findNoteToUnfavoriteRequestIds.end())
    {
        Q_UNUSED(m_findNoteToUnfavoriteRequestIds.erase(unfavoriteIt))
        m_noteCache.put(note.localUid(), note);
        unfavoriteNote(note.localUid());
    }
}

void FavoritesModel::onFindNoteFailed(Note note, bool withResourceMetadata, bool withResourceBinaryData,
                                      ErrorString errorDescription, QUuid requestId)
{
    auto restoreUpdateIt = m_findNoteToRestoreFailedUpdateRequestIds.find(requestId);
    auto performUpdateIt = m_findNoteToPerformUpdateRequestIds.find(requestId);
    auto unfavoriteIt = m_findNoteToUnfavoriteRequestIds.find(requestId);

    if ((restoreUpdateIt == m_findNoteToRestoreFailedUpdateRequestIds.end()) &&
        (performUpdateIt == m_findNoteToPerformUpdateRequestIds.end()) &&
        (unfavoriteIt == m_findNoteToUnfavoriteRequestIds.end()))
    {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onFindNoteFailed: note = ") << note << QStringLiteral(", with resource metadata = ")
            << (withResourceMetadata ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", with resource binary data = ")
            << (withResourceBinaryData ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", error description: ")
            << errorDescription << QStringLiteral(", request id = ") << requestId);

    if (restoreUpdateIt != m_findNoteToRestoreFailedUpdateRequestIds.end()) {
        Q_UNUSED(m_findNoteToRestoreFailedUpdateRequestIds.erase(restoreUpdateIt))
    }
    else if (performUpdateIt != m_findNoteToPerformUpdateRequestIds.end()) {
        Q_UNUSED(m_findNoteToPerformUpdateRequestIds.erase(performUpdateIt))
    }
    else if (unfavoriteIt != m_findNoteToUnfavoriteRequestIds.end()) {
        Q_UNUSED(m_findNoteToUnfavoriteRequestIds.erase(unfavoriteIt))
    }

    Q_EMIT notifyError(errorDescription);
}

void FavoritesModel::onListNotesComplete(LocalStorageManager::ListObjectsOptions flag, bool withResourceMetadata,
                                         bool withResourceBinaryData, size_t limit, size_t offset,
                                         LocalStorageManager::ListNotesOrder::type order,
                                         LocalStorageManager::OrderDirection::type orderDirection, QString linkedNotebookGuid,
                                         QList<Note> foundNotes, QUuid requestId)
{
    if (requestId != m_listNotesRequestId) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onListNotesComplete: flag = ") << flag << QStringLiteral(", with resource metadata = ")
            << (withResourceMetadata ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", with resource binary data = ")
            << (withResourceBinaryData ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", limit = ")
            << limit << QStringLiteral(", offset = ") << offset << QStringLiteral(", order = ") << order << QStringLiteral(", direction = ")
            << orderDirection << QStringLiteral(", linked notebook guid = ") << linkedNotebookGuid
            << QStringLiteral(", num found notes = ") << foundNotes.size() << QStringLiteral(", request id = ") << requestId);

    for(auto it = foundNotes.begin(), end = foundNotes.end(); it != end; ++it) {
        onNoteAddedOrUpdated(*it);
    }

    m_listNotesRequestId = QUuid();

    if (!foundNotes.isEmpty()) {
        QNTRACE(QStringLiteral("The number of found notes is greater than zero, requesting more notes from the local storage"));
        m_listNotesOffset += static_cast<size_t>(foundNotes.size());
        requestNotesList();
        return;
    }

    checkAllItemsListed();
}

void FavoritesModel::onListNotesFailed(LocalStorageManager::ListObjectsOptions flag, bool withResourceMetadata,
                                       bool withResourceBinaryData, size_t limit, size_t offset,
                                       LocalStorageManager::ListNotesOrder::type order,
                                       LocalStorageManager::OrderDirection::type orderDirection, QString linkedNotebookGuid,
                                       ErrorString errorDescription, QUuid requestId)
{
    if (requestId != m_listNotesRequestId) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onListNotesFailed: flag = ") << flag << QStringLiteral(", with resource metadata = ")
            << (withResourceMetadata ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", with resource binary data = ")
            << (withResourceBinaryData ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", limit = ")
            << limit << QStringLiteral(", offset = ") << offset << QStringLiteral(", order = ") << order << QStringLiteral(", direction = ")
            << orderDirection << QStringLiteral(", linked notebook guid = ") << linkedNotebookGuid
            << QStringLiteral(", error description = ") << errorDescription << QStringLiteral(", request id = ") << requestId);

    m_listNotesRequestId = QUuid();

    Q_EMIT notifyError(errorDescription);
}

void FavoritesModel::onExpungeNoteComplete(Note note, QUuid requestId)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onExpungeNoteComplete: note = ") << note << QStringLiteral("\nRequest id = ")
            << requestId);

    removeItemByLocalUid(note.localUid());

    checkAndUpdateNoteCountPerNotebookAfterNoteExpunge(note);
    checkAndUpdateNoteCountPerTagAfterNoteExpunge(note);
}

void FavoritesModel::onAddNotebookComplete(Notebook notebook, QUuid requestId)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onAddNotebookComplete: notebook = ") << notebook << QStringLiteral(", request id = ")
            << requestId);
    onNotebookAddedOrUpdated(notebook);
}

void FavoritesModel::onUpdateNotebookComplete(Notebook notebook, QUuid requestId)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onUpdateNotebookComplete: notebook = ") << notebook
            << QStringLiteral(", request id = ") << requestId);

    auto it = m_updateNotebookRequestIds.find(requestId);
    if (it != m_updateNotebookRequestIds.end()) {
        Q_UNUSED(m_updateNotebookRequestIds.erase(it))
        return;
    }

    onNotebookAddedOrUpdated(notebook);
}

void FavoritesModel::onUpdateNotebookFailed(Notebook notebook, ErrorString errorDescription, QUuid requestId)
{
    auto it = m_updateNotebookRequestIds.find(requestId);
    if (it == m_updateNotebookRequestIds.end()) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onUpdateNotebookFailed: notebook = ") << notebook << QStringLiteral("\nError description = ")
            << errorDescription << QStringLiteral(", request id = ") << requestId);

    Q_UNUSED(m_updateNotebookRequestIds.erase(it))

    requestId = QUuid::createUuid();
    Q_UNUSED(m_findNotebookToRestoreFailedUpdateRequestIds.insert(requestId))
    QNTRACE(QStringLiteral("Emitting the request to find a notebook: local uid = ") << notebook.localUid()
            << QStringLiteral(", request id = ") << requestId);
    Q_EMIT findNotebook(notebook, requestId);
}

void FavoritesModel::onFindNotebookComplete(Notebook notebook, QUuid requestId)
{
    auto restoreUpdateIt = m_findNotebookToRestoreFailedUpdateRequestIds.find(requestId);
    auto performUpdateIt = m_findNotebookToPerformUpdateRequestIds.find(requestId);
    auto unfavoriteIt = m_findNotebookToUnfavoriteRequestIds.find(requestId);

    if ((restoreUpdateIt == m_findNotebookToRestoreFailedUpdateRequestIds.end()) &&
        (performUpdateIt == m_findNotebookToPerformUpdateRequestIds.end()) &&
        (unfavoriteIt == m_findNotebookToUnfavoriteRequestIds.end()))
    {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onFindNotebookComplete: notebook = ") << notebook << QStringLiteral("\nRequest id = ")
            << requestId);

    if (restoreUpdateIt != m_findNotebookToRestoreFailedUpdateRequestIds.end())
    {
        Q_UNUSED(m_findNotebookToRestoreFailedUpdateRequestIds.erase(restoreUpdateIt))
        onNotebookAddedOrUpdated(notebook);
    }
    else if (performUpdateIt != m_findNotebookToPerformUpdateRequestIds.end())
    {
        Q_UNUSED(m_findNotebookToPerformUpdateRequestIds.erase(performUpdateIt))
        m_notebookCache.put(notebook.localUid(), notebook);
        FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
        auto it = localUidIndex.find(notebook.localUid());
        if (it != localUidIndex.end()) {
            updateItemInLocalStorage(*it);
        }
    }
    else if (unfavoriteIt != m_findNotebookToUnfavoriteRequestIds.end())
    {
        Q_UNUSED(m_findNotebookToPerformUpdateRequestIds.erase(performUpdateIt))
        m_notebookCache.put(notebook.localUid(), notebook);
        unfavoriteNotebook(notebook.localUid());
    }
}

void FavoritesModel::onFindNotebookFailed(Notebook notebook, ErrorString errorDescription, QUuid requestId)
{
    auto restoreUpdateIt = m_findNotebookToRestoreFailedUpdateRequestIds.find(requestId);
    auto performUpdateIt = m_findNotebookToPerformUpdateRequestIds.find(requestId);
    auto unfavoriteIt = m_findNotebookToUnfavoriteRequestIds.find(requestId);

    if ((restoreUpdateIt == m_findNotebookToRestoreFailedUpdateRequestIds.end()) &&
        (performUpdateIt == m_findNotebookToPerformUpdateRequestIds.end()) &&
        (unfavoriteIt == m_findNotebookToUnfavoriteRequestIds.end()))
    {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onFindNotebookFailed: notebook = ") << notebook << QStringLiteral("\nError description = ")
            << errorDescription << QStringLiteral(", request id = ") << requestId);

    if (restoreUpdateIt != m_findNotebookToRestoreFailedUpdateRequestIds.end()) {
        Q_UNUSED(m_findNotebookToRestoreFailedUpdateRequestIds.erase(restoreUpdateIt))
    }
    else if (performUpdateIt != m_findNotebookToPerformUpdateRequestIds.end()) {
        Q_UNUSED(m_findNotebookToPerformUpdateRequestIds.erase(performUpdateIt))
    }
    else if (unfavoriteIt != m_findNotebookToUnfavoriteRequestIds.end()) {
        Q_UNUSED(m_findNotebookToUnfavoriteRequestIds.erase(unfavoriteIt))
    }

    Q_EMIT notifyError(errorDescription);
}

void FavoritesModel::onListNotebooksComplete(LocalStorageManager::ListObjectsOptions flag,
                                             size_t limit, size_t offset,
                                             LocalStorageManager::ListNotebooksOrder::type order,
                                             LocalStorageManager::OrderDirection::type orderDirection,
                                             QString linkedNotebookGuid, QList<Notebook> foundNotebooks,
                                             QUuid requestId)
{
    if (requestId != m_listNotebooksRequestId) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onListNotebooksComplete: flag = ") << flag << QStringLiteral(", limit = ")
            << limit << QStringLiteral(", offset = ") << offset << QStringLiteral(", order = ") << order
            << QStringLiteral(", direction = ") << orderDirection << QStringLiteral(", linked notebook guid = ")
            << (linkedNotebookGuid.isNull() ? QStringLiteral("<null>") : linkedNotebookGuid) << QStringLiteral(", num found notebooks = ")
            << foundNotebooks.size() << QStringLiteral(", request id = ") << requestId);

    for(auto it = foundNotebooks.begin(), end = foundNotebooks.end(); it != end; ++it) {
        onNotebookAddedOrUpdated(*it);
    }

    m_listNotebooksRequestId = QUuid();

    if (!foundNotebooks.isEmpty()) {
        QNTRACE(QStringLiteral("The number of found notebooks is greater than zero, requesting more notebooks from the local storage"));
        m_listNotebooksOffset += static_cast<size_t>(foundNotebooks.size());
        requestNotebooksList();
        return;
    }

    checkAllItemsListed();
}

void FavoritesModel::onListNotebooksFailed(LocalStorageManager::ListObjectsOptions flag,
                                           size_t limit, size_t offset,
                                           LocalStorageManager::ListNotebooksOrder::type order,
                                           LocalStorageManager::OrderDirection::type orderDirection,
                                           QString linkedNotebookGuid, ErrorString errorDescription, QUuid requestId)
{
    if (requestId != m_listNotebooksRequestId) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onListNotebooksFailed: flag = ") << flag << QStringLiteral(", limit = ")
            << limit << QStringLiteral(", offset = ") << offset << QStringLiteral(", order = ") << order
            << QStringLiteral(", direction = ") << orderDirection << QStringLiteral(", linked notebook guid = ")
            << (linkedNotebookGuid.isNull() ? QStringLiteral("<null>") : linkedNotebookGuid) << QStringLiteral(", error description = ")
            << errorDescription << QStringLiteral(", request id = ") << requestId);

    m_listNotebooksRequestId = QUuid();

    Q_EMIT notifyError(errorDescription);
}

void FavoritesModel::onExpungeNotebookComplete(Notebook notebook, QUuid requestId)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onExpungeNotebookComplete: notebook = ") << notebook
            << QStringLiteral("\nRequest id = ") << requestId);
    removeItemByLocalUid(notebook.localUid());
}

void FavoritesModel::onAddTagComplete(Tag tag, QUuid requestId)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onAddTagComplete: tag = ") << tag << QStringLiteral("\nRequest id = ") << requestId);
    onTagAddedOrUpdated(tag);
}

void FavoritesModel::onUpdateTagComplete(Tag tag, QUuid requestId)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onUpdateTagComplete: tag = ") << tag << QStringLiteral("\nRequest id = ") << requestId);

    auto it = m_updateTagRequestIds.find(requestId);
    if (it != m_updateTagRequestIds.end()) {
        Q_UNUSED(m_updateTagRequestIds.erase(it))
        return;
    }

    onTagAddedOrUpdated(tag);
}

void FavoritesModel::onUpdateTagFailed(Tag tag, ErrorString errorDescription, QUuid requestId)
{
    auto it = m_updateTagRequestIds.find(requestId);
    if (it == m_updateTagRequestIds.end()) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onUpdateTagFailed: tag = ") << tag << QStringLiteral("\nError description = ")
            << errorDescription << QStringLiteral(", request id = ") << requestId);

    Q_UNUSED(m_updateTagRequestIds.erase(it))

    requestId = QUuid::createUuid();
    Q_UNUSED(m_findTagToRestoreFailedUpdateRequestIds.insert(requestId))
    QNTRACE(QStringLiteral("Emitting the request to find a tag: local uid = ") << tag.localUid()
            << QStringLiteral(", request id = ") << requestId);
    Q_EMIT findTag(tag, requestId);
}

void FavoritesModel::onFindTagComplete(Tag tag, QUuid requestId)
{
    auto restoreUpdateIt = m_findTagToRestoreFailedUpdateRequestIds.find(requestId);
    auto performUpdateIt = m_findTagToPerformUpdateRequestIds.find(requestId);
    auto unfavoriteIt = m_findTagToUnfavoriteRequestIds.find(requestId);

    if ((restoreUpdateIt == m_findTagToRestoreFailedUpdateRequestIds.end()) &&
        (performUpdateIt == m_findTagToPerformUpdateRequestIds.end()) &&
        (unfavoriteIt == m_findTagToUnfavoriteRequestIds.end()))
    {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onFindTagComplete: tag = ") << tag << QStringLiteral("\nRequest id = ") << requestId);

    if (restoreUpdateIt != m_findTagToRestoreFailedUpdateRequestIds.end())
    {
        Q_UNUSED(m_findTagToRestoreFailedUpdateRequestIds.erase(restoreUpdateIt))
        onTagAddedOrUpdated(tag);
    }
    else if (performUpdateIt != m_findTagToPerformUpdateRequestIds.end())
    {
        Q_UNUSED(m_findTagToPerformUpdateRequestIds.erase(performUpdateIt))
        m_tagCache.put(tag.localUid(), tag);
        FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
        auto it = localUidIndex.find(tag.localUid());
        if (it != localUidIndex.end()) {
            updateItemInLocalStorage(*it);
        }
    }
    else if (unfavoriteIt != m_findTagToUnfavoriteRequestIds.end())
    {
        Q_UNUSED(m_findTagToPerformUpdateRequestIds.erase(performUpdateIt))
        m_tagCache.put(tag.localUid(), tag);
        unfavoriteTag(tag.localUid());
    }
}

void FavoritesModel::onFindTagFailed(Tag tag, ErrorString errorDescription, QUuid requestId)
{

    auto restoreUpdateIt = m_findTagToRestoreFailedUpdateRequestIds.find(requestId);
    auto performUpdateIt = m_findTagToPerformUpdateRequestIds.find(requestId);
    auto unfavoriteIt = m_findTagToUnfavoriteRequestIds.find(requestId);

    if ((restoreUpdateIt == m_findTagToRestoreFailedUpdateRequestIds.end()) &&
        (performUpdateIt == m_findTagToPerformUpdateRequestIds.end()) &&
        (unfavoriteIt == m_findTagToUnfavoriteRequestIds.end()))
    {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onFindTagFailed: tag = ") << tag << QStringLiteral("\nError description = ")
            << errorDescription << QStringLiteral(", request id = ") << requestId);

    if (restoreUpdateIt != m_findTagToRestoreFailedUpdateRequestIds.end()) {
        Q_UNUSED(m_findTagToRestoreFailedUpdateRequestIds.erase(restoreUpdateIt))
    }
    else if (performUpdateIt != m_findTagToPerformUpdateRequestIds.end()) {
        Q_UNUSED(m_findTagToPerformUpdateRequestIds.erase(performUpdateIt))
    }
    else if (unfavoriteIt != m_findTagToUnfavoriteRequestIds.end()) {
        Q_UNUSED(m_findTagToUnfavoriteRequestIds.erase(unfavoriteIt))
    }

    Q_EMIT notifyError(errorDescription);
}

void FavoritesModel::onListTagsComplete(LocalStorageManager::ListObjectsOptions flag,
                                        size_t limit, size_t offset,
                                        LocalStorageManager::ListTagsOrder::type order,
                                        LocalStorageManager::OrderDirection::type orderDirection,
                                        QString linkedNotebookGuid, QList<Tag> foundTags, QUuid requestId)
{
    if (requestId != m_listTagsRequestId) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onListTagsComplete: flag = ") << flag << QStringLiteral(", limit = ")
            << limit << QStringLiteral(", offset = ") << offset << QStringLiteral(", order = ") << order
            << QStringLiteral(", direction = ") << orderDirection << QStringLiteral(", linked notebook guid = ")
            << (linkedNotebookGuid.isNull() ? QStringLiteral("<null>") : linkedNotebookGuid)
            << QStringLiteral(", num found tags = ") << foundTags.size() << QStringLiteral(", request id = ") << requestId);

    for(auto it = foundTags.begin(), end = foundTags.end(); it != end; ++it) {
        onTagAddedOrUpdated(*it);
    }

    m_listTagsRequestId = QUuid();

    if (!foundTags.isEmpty()) {
        QNTRACE(QStringLiteral("The number of found tags is greater than zero, requesting more tags from the local storage"));
        m_listTagsOffset += static_cast<size_t>(foundTags.size());
        requestTagsList();
        return;
    }

    checkAllItemsListed();
}

void FavoritesModel::onListTagsFailed(LocalStorageManager::ListObjectsOptions flag,
                                      size_t limit, size_t offset,
                                      LocalStorageManager::ListTagsOrder::type order,
                                      LocalStorageManager::OrderDirection::type orderDirection,
                                      QString linkedNotebookGuid, ErrorString errorDescription, QUuid requestId)
{
    if (requestId != m_listTagsRequestId) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onListTagsFailed: flag = ") << flag << QStringLiteral(", limit = ") << limit
            << QStringLiteral(", offset = ") << offset << QStringLiteral(", order = ") << order << QStringLiteral(", direction = ")
            << orderDirection << QStringLiteral(", linked notebook guid = ")
            << (linkedNotebookGuid.isNull() ? QStringLiteral("<null>") : linkedNotebookGuid)
            << QStringLiteral(", error description = ") << errorDescription << QStringLiteral(", request id = ") << requestId);

    m_listTagsRequestId = QUuid();

    Q_EMIT notifyError(errorDescription);
}

void FavoritesModel::onExpungeTagComplete(Tag tag, QStringList expungedChildTagLocalUids, QUuid requestId)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onExpungeTagComplete: tag = ") << tag
            << QStringLiteral("\nExpunged child tag local uids: ") << expungedChildTagLocalUids.join(QStringLiteral(", "))
            << QStringLiteral(", request id = ") << requestId);

    for(auto it = expungedChildTagLocalUids.constBegin(), end = expungedChildTagLocalUids.constEnd(); it != end; ++it) {
        removeItemByLocalUid(*it);
    }

    removeItemByLocalUid(tag.localUid());
}

void FavoritesModel::onAddSavedSearchComplete(SavedSearch search, QUuid requestId)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onAddSavedSearchComplete: ") << search << QStringLiteral("\nRequest id = ") << requestId);
    onSavedSearchAddedOrUpdated(search);
}

void FavoritesModel::onUpdateSavedSearchComplete(SavedSearch search, QUuid requestId)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onUpdateSavedSearchComplete: ") << search << QStringLiteral("\nRequest id = ") << requestId);

    auto it = m_updateSavedSearchRequestIds.find(requestId);
    if (it != m_updateSavedSearchRequestIds.end()) {
        Q_UNUSED(m_updateSavedSearchRequestIds.erase(it))
        return;
    }

    onSavedSearchAddedOrUpdated(search);
}

void FavoritesModel::onUpdateSavedSearchFailed(SavedSearch search, ErrorString errorDescription, QUuid requestId)
{
    auto it = m_updateSavedSearchRequestIds.find(requestId);
    if (it == m_updateSavedSearchRequestIds.end()) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onUpdateSavedSearchFailed: search = ") << search << QStringLiteral("\nError description = ")
            << errorDescription << QStringLiteral(", request id = ") << requestId);

    Q_UNUSED(m_updateSavedSearchRequestIds.erase(it))

    requestId = QUuid::createUuid();
    Q_UNUSED(m_findSavedSearchToRestoreFailedUpdateRequestIds.insert(requestId))
    QNTRACE(QStringLiteral("Emitting the request to find the saved search: local uid = ") << search.localUid()
            << QStringLiteral(", request id = ") << requestId);
    Q_EMIT findSavedSearch(search, requestId);
}

void FavoritesModel::onFindSavedSearchComplete(SavedSearch search, QUuid requestId)
{
    auto restoreUpdateIt = m_findSavedSearchToRestoreFailedUpdateRequestIds.find(requestId);
    auto performUpdateIt = m_findSavedSearchToPerformUpdateRequestIds.find(requestId);
    auto unfavoriteIt = m_findSavedSearchToUnfavoriteRequestIds.find(requestId);

    if ( (restoreUpdateIt == m_findSavedSearchToRestoreFailedUpdateRequestIds.end()) &&
         (performUpdateIt == m_findSavedSearchToPerformUpdateRequestIds.end()) &&
         (unfavoriteIt == m_findSavedSearchToUnfavoriteRequestIds.end()) )
    {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onFindSavedSearchComplete: search = ") << search
            << QStringLiteral("\nRequest id = ") << requestId);

    if (restoreUpdateIt != m_findSavedSearchToRestoreFailedUpdateRequestIds.end())
    {
        Q_UNUSED(m_findSavedSearchToRestoreFailedUpdateRequestIds.erase(restoreUpdateIt))
        onSavedSearchAddedOrUpdated(search);
    }
    else if (performUpdateIt != m_findSavedSearchToPerformUpdateRequestIds.end())
    {
        Q_UNUSED(m_findSavedSearchToPerformUpdateRequestIds.erase(performUpdateIt))
        m_savedSearchCache.put(search.localUid(), search);
        FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
        auto it = localUidIndex.find(search.localUid());
        if (it != localUidIndex.end()) {
            updateItemInLocalStorage(*it);
        }
    }
    else if (unfavoriteIt != m_findSavedSearchToUnfavoriteRequestIds.end())
    {
        Q_UNUSED(m_findSavedSearchToPerformUpdateRequestIds.erase(performUpdateIt))
        m_savedSearchCache.put(search.localUid(), search);
        unfavoriteSavedSearch(search.localUid());
    }
}

void FavoritesModel::onFindSavedSearchFailed(SavedSearch search, ErrorString errorDescription, QUuid requestId)
{
    auto restoreUpdateIt = m_findSavedSearchToRestoreFailedUpdateRequestIds.find(requestId);
    auto performUpdateIt = m_findSavedSearchToPerformUpdateRequestIds.find(requestId);
    auto unfavoriteIt = m_findSavedSearchToUnfavoriteRequestIds.find(requestId);

    if ( (restoreUpdateIt == m_findSavedSearchToRestoreFailedUpdateRequestIds.end()) &&
         (performUpdateIt == m_findSavedSearchToPerformUpdateRequestIds.end()) &&
         (unfavoriteIt == m_findSavedSearchToUnfavoriteRequestIds.end()) )
    {
        return;
    }

    QNWARNING(QStringLiteral("FavoritesModel::onFindSavedSearchFailed: search = ") << search << QStringLiteral("\nError description = ")
              << errorDescription << QStringLiteral(", request id = ") << requestId);

    if (restoreUpdateIt != m_findSavedSearchToRestoreFailedUpdateRequestIds.end()) {
        Q_UNUSED(m_findSavedSearchToRestoreFailedUpdateRequestIds.erase(restoreUpdateIt))
    }
    else if (performUpdateIt != m_findSavedSearchToPerformUpdateRequestIds.end()) {
        Q_UNUSED(m_findSavedSearchToPerformUpdateRequestIds.erase(performUpdateIt))
    }
    else if (unfavoriteIt != m_findSavedSearchToUnfavoriteRequestIds.end()) {
        Q_UNUSED(m_findSavedSearchToUnfavoriteRequestIds.erase(unfavoriteIt))
    }

    Q_EMIT notifyError(errorDescription);
}

void FavoritesModel::onListSavedSearchesComplete(LocalStorageManager::ListObjectsOptions flag,
                                                 size_t limit, size_t offset,
                                                 LocalStorageManager::ListSavedSearchesOrder::type order,
                                                 LocalStorageManager::OrderDirection::type orderDirection,
                                                 QList<SavedSearch> foundSearches, QUuid requestId)
{
    if (requestId != m_listSavedSearchesRequestId) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onListSavedSearchesComplete: flag = ") << flag << QStringLiteral(", limit = ")
            << limit << QStringLiteral(", offset = ") << offset << QStringLiteral(", order = ") << order
            << QStringLiteral(", direction = ") << orderDirection << QStringLiteral(", num found searches = ")
            << foundSearches.size() << QStringLiteral(", request id = ") << requestId);

    for(auto it = foundSearches.begin(), end = foundSearches.end(); it != end; ++it) {
        onSavedSearchAddedOrUpdated(*it);
    }

    m_listSavedSearchesRequestId = QUuid();

    if (!foundSearches.isEmpty()) {
        QNTRACE(QStringLiteral("The number of found saved searches is not empty, requesting more saved searches from the local storage"));
        m_listSavedSearchesOffset += static_cast<size_t>(foundSearches.size());
        requestSavedSearchesList();
        return;
    }

    checkAllItemsListed();
}

void FavoritesModel::onListSavedSearchesFailed(LocalStorageManager::ListObjectsOptions flag,
                                               size_t limit, size_t offset,
                                               LocalStorageManager::ListSavedSearchesOrder::type order,
                                               LocalStorageManager::OrderDirection::type orderDirection,
                                               ErrorString errorDescription, QUuid requestId)
{
    if (requestId != m_listSavedSearchesRequestId) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onListSavedSearchesFailed: flag = ") << flag << QStringLiteral(", limit = ")
            << limit << QStringLiteral(", offset = ") << offset << QStringLiteral(", order = ") << order << QStringLiteral(", direction = ")
            << orderDirection << QStringLiteral(", error: ") << errorDescription << QStringLiteral(", request id = ") << requestId);

    m_listSavedSearchesRequestId = QUuid();

    Q_EMIT notifyError(errorDescription);
}

void FavoritesModel::onExpungeSavedSearchComplete(SavedSearch search, QUuid requestId)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onExpungeSavedSearchComplete: search = ") << search
            << QStringLiteral("\nRequest id = ") << requestId);
    removeItemByLocalUid(search.localUid());
}

void FavoritesModel::onGetNoteCountPerNotebookComplete(int noteCount, Notebook notebook, QUuid requestId)
{
    auto it = m_notebookLocalUidToNoteCountRequestIdBimap.right.find(requestId);
    if (it == m_notebookLocalUidToNoteCountRequestIdBimap.right.end()) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onGetNoteCountPerNotebookComplete: note count = ") << noteCount
            << QStringLiteral(", notebook local uid = ") << notebook.localUid() << QStringLiteral(", request id = ") << requestId);

    Q_UNUSED(m_notebookLocalUidToNoteCountRequestIdBimap.right.erase(it))

    FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
    auto itemIt = localUidIndex.find(notebook.localUid());
    if (Q_UNLIKELY(itemIt == localUidIndex.end())) {
        QNDEBUG(QStringLiteral("Can't find the notebook item within the favorites model for which the note count was received"));
        return;
    }

    FavoritesModelItem item = *itemIt;
    item.setNumNotesTargeted(noteCount);
    Q_UNUSED(localUidIndex.replace(itemIt, item))
    updateItemColumnInView(item, Columns::NumNotesTargeted);
}

void FavoritesModel::onGetNoteCountPerNotebookFailed(ErrorString errorDescription, Notebook notebook, QUuid requestId)
{
    auto it = m_notebookLocalUidToNoteCountRequestIdBimap.right.find(requestId);
    if (it == m_notebookLocalUidToNoteCountRequestIdBimap.right.end()) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onGetNoteCountPerNotebookFailed: error description = ") << errorDescription
            << QStringLiteral("\nNotebook local uid = ") << notebook.localUid() << QStringLiteral(", request id = ") << requestId);

    Q_UNUSED(m_notebookLocalUidToNoteCountRequestIdBimap.right.erase(it))

    QNWARNING(errorDescription << QStringLiteral(", notebook: ") << notebook);
    Q_EMIT notifyError(errorDescription);
}

void FavoritesModel::onGetNoteCountPerTagComplete(int noteCount, Tag tag, QUuid requestId)
{
    auto it = m_tagLocalUidToNoteCountRequestIdBimap.right.find(requestId);
    if (it == m_tagLocalUidToNoteCountRequestIdBimap.right.end()) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onGetNoteCountPerTagComplete: note count = ") << noteCount
            << QStringLiteral(", tag local uid = ") << tag.localUid() << QStringLiteral(", request id = ") << requestId);

    Q_UNUSED(m_tagLocalUidToNoteCountRequestIdBimap.right.erase(it))

    FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
    auto itemIt = localUidIndex.find(tag.localUid());
    if (Q_UNLIKELY(itemIt == localUidIndex.end())) {
        QNDEBUG(QStringLiteral("Can't find the tag item within the favorites model for which the note count was received"));
        return;
    }

    FavoritesModelItem item = *itemIt;
    item.setNumNotesTargeted(noteCount);
    Q_UNUSED(localUidIndex.replace(itemIt, item))
    updateItemColumnInView(item, Columns::NumNotesTargeted);
}

void FavoritesModel::onGetNoteCountPerTagFailed(ErrorString errorDescription, Tag tag, QUuid requestId)
{
    auto it = m_tagLocalUidToNoteCountRequestIdBimap.right.find(requestId);
    if (it == m_tagLocalUidToNoteCountRequestIdBimap.right.end()) {
        return;
    }

    QNDEBUG(QStringLiteral("FavoritesModel::onGetNoteCountPerTagFailed: error description = ") << errorDescription
            << QStringLiteral("\nTag local uid = ") << tag.localUid() << QStringLiteral(", request id = ") << requestId);

    Q_UNUSED(m_tagLocalUidToNoteCountRequestIdBimap.right.erase(it))

    QNWARNING(errorDescription << QStringLiteral(", tag: ") << tag);
    Q_EMIT notifyError(errorDescription);
}

void FavoritesModel::createConnections(const NoteModel & noteModel, LocalStorageManagerAsync & localStorageManagerAsync)
{
    QNDEBUG(QStringLiteral("FavoritesModel::createConnections"));

    if (!noteModel.allNotesListed()) {
        QObject::connect(&noteModel, QNSIGNAL(NoteModel,notifyAllNotesListed),
                         this, QNSLOT(FavoritesModel,onAllNotesListed));
    }

    // Local signals to localStorageManagerAsync's slots
    QObject::connect(this, QNSIGNAL(FavoritesModel,updateNote,Note,bool,bool,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onUpdateNoteRequest,Note,bool,bool,QUuid));
    QObject::connect(this, QNSIGNAL(FavoritesModel,findNote,Note,bool,bool,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onFindNoteRequest,Note,bool,bool,QUuid));
    QObject::connect(this, QNSIGNAL(FavoritesModel,listNotes,LocalStorageManager::ListObjectsOptions,bool,bool,size_t,size_t,
                                    LocalStorageManager::ListNotesOrder::type,LocalStorageManager::OrderDirection::type,QString,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onListNotesRequest,
                                                       LocalStorageManager::ListObjectsOptions,bool,bool,size_t,size_t,
                                                       LocalStorageManager::ListNotesOrder::type,
                                                       LocalStorageManager::OrderDirection::type,QString,QUuid));
    QObject::connect(this, QNSIGNAL(FavoritesModel,updateNotebook,Notebook,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onUpdateNotebookRequest,Notebook,QUuid));
    QObject::connect(this, QNSIGNAL(FavoritesModel,findNotebook,Notebook,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onFindNotebookRequest,Notebook,QUuid));
    QObject::connect(this, QNSIGNAL(FavoritesModel,listNotebooks,LocalStorageManager::ListObjectsOptions,
                                    size_t,size_t,LocalStorageManager::ListNotebooksOrder::type,
                                    LocalStorageManager::OrderDirection::type,QString,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onListNotebooksRequest,
                                                       LocalStorageManager::ListObjectsOptions,
                                                       size_t,size_t,LocalStorageManager::ListNotebooksOrder::type,
                                                       LocalStorageManager::OrderDirection::type,QString,QUuid));
    QObject::connect(this, QNSIGNAL(FavoritesModel,updateTag,Tag,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onUpdateTagRequest,Tag,QUuid));
    QObject::connect(this, QNSIGNAL(FavoritesModel,findTag,Tag,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onFindTagRequest,Tag,QUuid));
    QObject::connect(this, QNSIGNAL(FavoritesModel,listTags,LocalStorageManager::ListObjectsOptions,size_t,size_t,
                                    LocalStorageManager::ListTagsOrder::type,LocalStorageManager::OrderDirection::type,QString,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onListTagsRequest,LocalStorageManager::ListObjectsOptions,
                                                       size_t,size_t,LocalStorageManager::ListTagsOrder::type,
                                                       LocalStorageManager::OrderDirection::type,QString,QUuid));
    QObject::connect(this, QNSIGNAL(FavoritesModel,updateSavedSearch,SavedSearch,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onUpdateSavedSearchRequest,SavedSearch,QUuid));
    QObject::connect(this, QNSIGNAL(FavoritesModel,findSavedSearch,SavedSearch,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onFindSavedSearchRequest,SavedSearch,QUuid));
    QObject::connect(this, QNSIGNAL(FavoritesModel,listSavedSearches,LocalStorageManager::ListObjectsOptions,
                                    size_t,size_t,LocalStorageManager::ListSavedSearchesOrder::type,
                                    LocalStorageManager::OrderDirection::type,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onListSavedSearchesRequest,
                                                       LocalStorageManager::ListObjectsOptions,
                                                       size_t,size_t,LocalStorageManager::ListSavedSearchesOrder::type,
                                                       LocalStorageManager::OrderDirection::type,QUuid));
    QObject::connect(this, QNSIGNAL(FavoritesModel,noteCountPerNotebook,Notebook,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onGetNoteCountPerNotebookRequest,Notebook,QUuid));
    QObject::connect(this, QNSIGNAL(FavoritesModel,noteCountPerTag,Tag,QUuid),
                     &localStorageManagerAsync, QNSLOT(LocalStorageManagerAsync,onGetNoteCountPerTagRequest,Tag,QUuid));

    // localStorageManagerAsync's signals to local slots
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,addNoteComplete,Note,QUuid),
                     this, QNSLOT(FavoritesModel,onAddNoteComplete,Note,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,updateNoteComplete,Note,bool,bool,QUuid),
                     this, QNSLOT(FavoritesModel,onUpdateNoteComplete,Note,bool,bool,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,updateNoteFailed,Note,bool,bool,ErrorString,QUuid),
                     this, QNSLOT(FavoritesModel,onUpdateNoteFailed,Note,bool,bool,ErrorString,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,findNoteComplete,Note,bool,bool,QUuid),
                     this, QNSLOT(FavoritesModel,onFindNoteComplete,Note,bool,bool,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,findNoteFailed,Note,bool,bool,ErrorString,QUuid),
                     this, QNSLOT(FavoritesModel,onFindNoteFailed,Note,bool,bool,ErrorString,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,listNotesComplete,LocalStorageManager::ListObjectsOptions,bool,bool,size_t,size_t,
                                                                LocalStorageManager::ListNotesOrder::type,LocalStorageManager::OrderDirection::type,
                                                                QString,QList<Note>,QUuid),
                     this, QNSLOT(FavoritesModel,onListNotesComplete,LocalStorageManager::ListObjectsOptions,bool,bool,size_t,size_t,
                                  LocalStorageManager::ListNotesOrder::type,LocalStorageManager::OrderDirection::type,QString,QList<Note>,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,listNotesFailed,LocalStorageManager::ListObjectsOptions,bool,bool,size_t,size_t,
                                                                LocalStorageManager::ListNotesOrder::type,LocalStorageManager::OrderDirection::type,QString,ErrorString,QUuid),
                     this, QNSLOT(FavoritesModel,onListNotesFailed,LocalStorageManager::ListObjectsOptions,bool,bool,size_t,size_t,
                                  LocalStorageManager::ListNotesOrder::type,LocalStorageManager::OrderDirection::type,QString,ErrorString,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,expungeNoteComplete,Note,QUuid),
                     this, QNSLOT(FavoritesModel,onExpungeNoteComplete,Note,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,addNotebookComplete,Notebook,QUuid),
                     this, QNSLOT(FavoritesModel,onAddNotebookComplete,Notebook,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,updateNotebookComplete,Notebook,QUuid),
                     this, QNSLOT(FavoritesModel,onUpdateNotebookComplete,Notebook,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,updateNotebookFailed,Notebook,ErrorString,QUuid),
                     this, QNSLOT(FavoritesModel,onUpdateNotebookFailed,Notebook,ErrorString,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,findNotebookComplete,Notebook,QUuid),
                     this, QNSLOT(FavoritesModel,onFindNotebookComplete,Notebook,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,findNotebookFailed,Notebook,ErrorString,QUuid),
                     this, QNSLOT(FavoritesModel,onFindNotebookFailed,Notebook,ErrorString,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,listNotebooksComplete,
                                                         LocalStorageManager::ListObjectsOptions,size_t,size_t,
                                                         LocalStorageManager::ListNotebooksOrder::type,
                                                         LocalStorageManager::OrderDirection::type,QString,
                                                         QList<Notebook>,QUuid),
                     this, QNSLOT(FavoritesModel,onListNotebooksComplete,LocalStorageManager::ListObjectsOptions,
                                  size_t,size_t,LocalStorageManager::ListNotebooksOrder::type,
                                  LocalStorageManager::OrderDirection::type,QString,QList<Notebook>,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,listNotebooksFailed,
                                                         LocalStorageManager::ListObjectsOptions,size_t,size_t,
                                                         LocalStorageManager::ListNotebooksOrder::type,
                                                         LocalStorageManager::OrderDirection::type,
                                                         QString,ErrorString,QUuid),
                     this, QNSLOT(FavoritesModel,onListNotebooksFailed,LocalStorageManager::ListObjectsOptions,
                                  size_t,size_t,LocalStorageManager::ListNotebooksOrder::type,
                                  LocalStorageManager::OrderDirection::type,QString,ErrorString,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,expungeNotebookComplete,Notebook,QUuid),
                     this, QNSLOT(FavoritesModel,onExpungeNotebookComplete,Notebook,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,addTagComplete,Tag,QUuid),
                     this, QNSLOT(FavoritesModel,onAddTagComplete,Tag,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,updateTagComplete,Tag,QUuid),
                     this, QNSLOT(FavoritesModel,onUpdateTagComplete,Tag,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,updateTagFailed,Tag,ErrorString,QUuid),
                     this, QNSLOT(FavoritesModel,onUpdateTagFailed,Tag,ErrorString,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,findTagComplete,Tag,QUuid),
                     this, QNSLOT(FavoritesModel,onFindTagComplete,Tag,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,findTagFailed,Tag,ErrorString,QUuid),
                     this, QNSLOT(FavoritesModel,onFindTagFailed,Tag,ErrorString,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,listTagsComplete,LocalStorageManager::ListObjectsOptions,
                                                         size_t,size_t,LocalStorageManager::ListTagsOrder::type,LocalStorageManager::OrderDirection::type,
                                                         QString,QList<Tag>,QUuid),
                     this, QNSLOT(FavoritesModel,onListTagsComplete,LocalStorageManager::ListObjectsOptions,size_t,size_t,
                                  LocalStorageManager::ListTagsOrder::type,LocalStorageManager::OrderDirection::type,QString,QList<Tag>,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,listTagsFailed,LocalStorageManager::ListObjectsOptions,
                                                         size_t,size_t,LocalStorageManager::ListTagsOrder::type,LocalStorageManager::OrderDirection::type,
                                                         QString,ErrorString,QUuid),
                     this, QNSLOT(FavoritesModel,onListTagsFailed,LocalStorageManager::ListObjectsOptions,size_t,size_t,
                                  LocalStorageManager::ListTagsOrder::type,LocalStorageManager::OrderDirection::type,QString,ErrorString,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,expungeTagComplete,Tag,QStringList,QUuid),
                     this, QNSLOT(FavoritesModel,onExpungeTagComplete,Tag,QStringList,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,addSavedSearchComplete,SavedSearch,QUuid),
                     this, QNSLOT(FavoritesModel,onAddSavedSearchComplete,SavedSearch,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,updateSavedSearchComplete,SavedSearch,QUuid),
                     this, QNSLOT(FavoritesModel,onUpdateSavedSearchComplete,SavedSearch,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,updateSavedSearchFailed,SavedSearch,ErrorString,QUuid),
                     this, QNSLOT(FavoritesModel,onUpdateSavedSearchFailed,SavedSearch,ErrorString,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,findSavedSearchComplete,SavedSearch,QUuid),
                     this, QNSLOT(FavoritesModel,onFindSavedSearchComplete,SavedSearch,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,findSavedSearchFailed,SavedSearch,ErrorString,QUuid),
                     this, QNSLOT(FavoritesModel,onFindSavedSearchFailed,SavedSearch,ErrorString,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,listSavedSearchesComplete,LocalStorageManager::ListObjectsOptions,
                                                         size_t,size_t,LocalStorageManager::ListSavedSearchesOrder::type,
                                                         LocalStorageManager::OrderDirection::type,QList<SavedSearch>,QUuid),
                     this, QNSLOT(FavoritesModel,onListSavedSearchesComplete,LocalStorageManager::ListObjectsOptions,
                                  size_t,size_t,LocalStorageManager::ListSavedSearchesOrder::type, LocalStorageManager::OrderDirection::type,
                                  QList<SavedSearch>,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,listSavedSearchesFailed,
                                                         LocalStorageManager::ListObjectsOptions,
                                                         size_t,size_t,LocalStorageManager::ListSavedSearchesOrder::type,
                                                         LocalStorageManager::OrderDirection::type,ErrorString,QUuid),
                     this, QNSLOT(FavoritesModel,onListSavedSearchesFailed,LocalStorageManager::ListObjectsOptions,
                                  size_t,size_t,LocalStorageManager::ListSavedSearchesOrder::type,LocalStorageManager::OrderDirection::type,
                                  ErrorString,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,expungeSavedSearchComplete,SavedSearch,QUuid),
                     this, QNSLOT(FavoritesModel,onExpungeSavedSearchComplete,SavedSearch,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,getNoteCountPerNotebookComplete,int,Notebook,QUuid),
                     this, QNSLOT(FavoritesModel,onGetNoteCountPerNotebookComplete,int,Notebook,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,getNoteCountPerNotebookFailed,ErrorString,Notebook,QUuid),
                     this, QNSLOT(FavoritesModel,onGetNoteCountPerNotebookFailed,ErrorString,Notebook,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,getNoteCountPerTagComplete,int,Tag,QUuid),
                     this, QNSLOT(FavoritesModel,onGetNoteCountPerTagComplete,int,Tag,QUuid));
    QObject::connect(&localStorageManagerAsync, QNSIGNAL(LocalStorageManagerAsync,getNoteCountPerTagFailed,ErrorString,Tag,QUuid),
                     this, QNSLOT(FavoritesModel,onGetNoteCountPerTagFailed,ErrorString,Tag,QUuid));
}

void FavoritesModel::requestNotesList()
{
    QNDEBUG(QStringLiteral("FavoritesModel::requestNotesList: offset = ") << m_listNotesOffset);

    LocalStorageManager::ListObjectsOptions flags = LocalStorageManager::ListFavoritedElements;
    LocalStorageManager::ListNotesOrder::type order = LocalStorageManager::ListNotesOrder::NoOrder;
    LocalStorageManager::OrderDirection::type direction = LocalStorageManager::OrderDirection::Ascending;

    m_listNotesRequestId = QUuid::createUuid();
    QNTRACE(QStringLiteral("Emitting the request to list notes: offset = ") << m_listNotesOffset << QStringLiteral(", request id = ") << m_listNotesRequestId);
    Q_EMIT listNotes(flags, /* with resource metadata = */ false, /* with resource binary data = */ false,
                     NOTE_LIST_LIMIT, m_listNotesOffset, order, direction, QString(), m_listNotesRequestId);
}

void FavoritesModel::requestNotebooksList()
{
    QNDEBUG(QStringLiteral("FavoritesModel::requestNotebooksList: offset = ") << m_listNotebooksOffset);

    // NOTE: the subscription to all notebooks is necessary in order to receive the information about the restrictions for various notebooks +
    // for the collection of notebook names to forbid any two notebooks within the account to have the same name in a case-insensitive manner
    LocalStorageManager::ListObjectsOptions flags = LocalStorageManager::ListAll;
    LocalStorageManager::ListNotebooksOrder::type order = LocalStorageManager::ListNotebooksOrder::NoOrder;
    LocalStorageManager::OrderDirection::type direction = LocalStorageManager::OrderDirection::Ascending;

    m_listNotebooksRequestId = QUuid::createUuid();
    QNTRACE(QStringLiteral("Emitting the request to list notebooks: offset = ") << m_listNotebooksOffset
            << QStringLiteral(", request id = ") << m_listNotebooksRequestId);
    Q_EMIT listNotebooks(flags, NOTEBOOK_LIST_LIMIT, m_listNotebooksOffset, order, direction, QString(), m_listNotebooksRequestId);
}

void FavoritesModel::requestTagsList()
{
    QNDEBUG(QStringLiteral("FavoritesModel::requestTagsList: offset = ") << m_listTagsOffset);

    // NOTE: the subscription to all tags is necessary for the collection of tag names to forbid any two tags
    // within the account to have the same name in a case-insensitive manner
    LocalStorageManager::ListObjectsOptions flags = LocalStorageManager::ListAll;
    LocalStorageManager::ListTagsOrder::type order = LocalStorageManager::ListTagsOrder::NoOrder;
    LocalStorageManager::OrderDirection::type direction = LocalStorageManager::OrderDirection::Ascending;

    m_listTagsRequestId = QUuid::createUuid();
    QNTRACE(QStringLiteral("Emitting the request to list tags: offset = ") << m_listTagsOffset
            << QStringLiteral(", request id = ") << m_listTagsRequestId);
    Q_EMIT listTags(flags, TAG_LIST_LIMIT, m_listTagsOffset, order, direction, QString(), m_listTagsRequestId);
}

void FavoritesModel::requestSavedSearchesList()
{
    QNDEBUG(QStringLiteral("FavoritesModel::requestSavedSearchesList: offset = ") << m_listSavedSearchesOffset);

    // NOTE: the subscription to all saved searches is necessary for the collection of saved search names to forbid any two saved searches
    // within the account to have the same name in a case-insensitive manner
    LocalStorageManager::ListObjectsOptions flags = LocalStorageManager::ListAll;
    LocalStorageManager::ListSavedSearchesOrder::type order = LocalStorageManager::ListSavedSearchesOrder::NoOrder;
    LocalStorageManager::OrderDirection::type direction = LocalStorageManager::OrderDirection::Ascending;

    m_listSavedSearchesRequestId = QUuid::createUuid();
    QNTRACE(QStringLiteral("Emitting the request to list saved searches: offset = ") << m_listSavedSearchesOffset
            << QStringLiteral(", request id = ") << m_listSavedSearchesRequestId);
    Q_EMIT listSavedSearches(flags, SAVED_SEARCH_LIST_LIMIT, m_listSavedSearchesOffset,
                           order, direction, m_listSavedSearchesRequestId);
}

void FavoritesModel::requestNoteCountForNotebook(const QString & notebookLocalUid, const NoteCountRequestOption::type option)
{
    QNDEBUG(QStringLiteral("FavoritesModel::requestNoteCountForNotebook: notebook lcoal uid = ") << notebookLocalUid
            << QStringLiteral(", note count request option = ") << option);

    if (option != NoteCountRequestOption::Force)
    {
        auto it = m_notebookLocalUidToNoteCountRequestIdBimap.left.find(notebookLocalUid);
        if (it != m_notebookLocalUidToNoteCountRequestIdBimap.left.end()) {
            QNDEBUG(QStringLiteral("There's an active request to fetch the note count for this notebook local uid"));
            return;
        }
    }

    QUuid requestId = QUuid::createUuid();
    m_notebookLocalUidToNoteCountRequestIdBimap.insert(LocalUidToRequestIdBimap::value_type(notebookLocalUid, requestId));
    Notebook dummyNotebook;
    dummyNotebook.setLocalUid(notebookLocalUid);
    QNTRACE(QStringLiteral("Emitting the request to get the note count per notebook: notebook local uid = ")
            << notebookLocalUid << QStringLiteral(", request id = ") << requestId);
    Q_EMIT noteCountPerNotebook(dummyNotebook, requestId);
}

void FavoritesModel::requestNoteCountForAllNotebooks(const NoteCountRequestOption::type option)
{
    QNDEBUG(QStringLiteral("FavoritesModel::requestNoteCountForAllNotebooks: note count request option = ")
            << option);

    const FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();

    for(auto it = localUidIndex.begin(), end = localUidIndex.end(); it != end; ++it)
    {
        const FavoritesModelItem & item = *it;
        if (item.type() != FavoritesModelItem::Type::Notebook) {
            continue;
        }

        requestNoteCountForNotebook(item.localUid(), option);
    }
}

void FavoritesModel::checkAndIncrementNoteCountPerNotebook(const QString & notebookLocalUid)
{
    QNDEBUG(QStringLiteral("FavoritesModel::checkAndIncrementNoteCountPerNotebook: ") << notebookLocalUid);
    checkAndAdjustNoteCountPerNotebook(notebookLocalUid, /* increment = */ true);
}

void FavoritesModel::checkAndDecrementNoteCountPerNotebook(const QString & notebookLocalUid)
{
    QNDEBUG(QStringLiteral("FavoritesModel::checkAndDecrementNoteCountPerNotebook: ") << notebookLocalUid);
    checkAndAdjustNoteCountPerNotebook(notebookLocalUid, /* increment = */ false);
}

void FavoritesModel::checkAndAdjustNoteCountPerNotebook(const QString & notebookLocalUid, const bool increment)
{
    auto requestIt = m_notebookLocalUidToNoteCountRequestIdBimap.left.find(notebookLocalUid);
    if (requestIt != m_notebookLocalUidToNoteCountRequestIdBimap.left.end()) {
        QNDEBUG(QStringLiteral("There's an active request to fetch the note count for notebook ")
                << notebookLocalUid << QStringLiteral(": ") << requestIt->second
                << QStringLiteral(", need to restart it to ensure the proper number of notes per notebook"));
        requestNoteCountForNotebook(notebookLocalUid, NoteCountRequestOption::Force);
        return;
    }

    FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
    auto notebookItemIt = localUidIndex.find(notebookLocalUid);
    if (notebookItemIt == localUidIndex.end()) {
        QNDEBUG(QStringLiteral("Notebook is not within the favorites model"));
        return;
    }

    FavoritesModelItem item = *notebookItemIt;
    int numNotesTargeted = item.numNotesTargeted();
    if (increment) {
        ++numNotesTargeted;
        QNDEBUG(QStringLiteral("Incremented to ") << numNotesTargeted);
    }
    else {
        --numNotesTargeted;
        QNDEBUG(QStringLiteral("Decremented to ") << numNotesTargeted);
    }
    item.setNumNotesTargeted(std::max(numNotesTargeted, 0));
    Q_UNUSED(localUidIndex.replace(notebookItemIt, item))

    updateItemColumnInView(item, Columns::NumNotesTargeted);
}

void FavoritesModel::requestNoteCountForTag(const QString & tagLocalUid, const NoteCountRequestOption::type option)
{
    QNDEBUG(QStringLiteral("FavoritesModel::requestNoteCountForTag: tag local uid = ") << tagLocalUid
            << QStringLiteral(", note count request option = ") << option);

    if (option != NoteCountRequestOption::Force)
    {
        auto it = m_tagLocalUidToNoteCountRequestIdBimap.left.find(tagLocalUid);
        if (it != m_tagLocalUidToNoteCountRequestIdBimap.left.end()) {
            QNDEBUG(QStringLiteral("There's an active request to fetch the note count for this tag local uid"));
            return;
        }
    }

    QUuid requestId = QUuid::createUuid();
    m_tagLocalUidToNoteCountRequestIdBimap.insert(LocalUidToRequestIdBimap::value_type(tagLocalUid, requestId));
    Tag dummyTag;
    dummyTag.setLocalUid(tagLocalUid);
    QNTRACE(QStringLiteral("Emitting the request to get the note count per tag: tag local uid = ") << tagLocalUid
            << QStringLiteral(", request id = ") << requestId);
    Q_EMIT noteCountPerTag(dummyTag, requestId);
}

void FavoritesModel::requestNoteCountForAllTags(const NoteCountRequestOption::type option)
{
    QNDEBUG(QStringLiteral("FavoritesModel::requestNoteCountForAllTags"));

    const FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();

    for(auto it = localUidIndex.begin(), end = localUidIndex.end(); it != end; ++it)
    {
        const FavoritesModelItem & item = *it;
        if (item.type() != FavoritesModelItem::Type::Tag) {
            continue;
        }

        requestNoteCountForTag(item.localUid(), option);
    }
}

void FavoritesModel::checkAndIncrementNoteCountPerTag(const QString & tagLocalUid)
{
    QNDEBUG(QStringLiteral("FavoritesModel::checkAndIncrementNoteCountPerTag: ") << tagLocalUid);
    checkAndAdjustNoteCountPerTag(tagLocalUid, /* increment = */ true);
}

void FavoritesModel::checkAndDecrementNoteCountPerTag(const QString & tagLocalUid)
{
    QNDEBUG(QStringLiteral("FavoritesModel::checkAndDecrementNoteCountPerTag: ") << tagLocalUid);
    checkAndAdjustNoteCountPerTag(tagLocalUid, /* increment = */ false);
}

void FavoritesModel::checkAndAdjustNoteCountPerTag(const QString & tagLocalUid, const bool increment)
{
    auto requestIt = m_tagLocalUidToNoteCountRequestIdBimap.left.find(tagLocalUid);
    if (requestIt != m_tagLocalUidToNoteCountRequestIdBimap.left.end()) {
        QNDEBUG(QStringLiteral("There's an active request to fetch the note count for tag ")
                << tagLocalUid << QStringLiteral(": ") << requestIt->second
                << QStringLiteral(", need to restart it to ensure the proper number of notes per tag"));
        requestNoteCountForTag(tagLocalUid, NoteCountRequestOption::Force);
        return;
    }

    FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
    auto tagItemIt = localUidIndex.find(tagLocalUid);
    if (tagItemIt == localUidIndex.end()) {
        QNDEBUG(QStringLiteral("Tag is not within the favorites model"));
        return;
    }

    FavoritesModelItem item = *tagItemIt;
    int numNotesTargeted = item.numNotesTargeted();
    if (increment) {
        ++numNotesTargeted;
        QNDEBUG(QStringLiteral("Incremented to ") << numNotesTargeted);
    }
    else {
        --numNotesTargeted;
        QNDEBUG(QStringLiteral("Decremented to ") << numNotesTargeted);
    }
    item.setNumNotesTargeted(std::max(numNotesTargeted, 0));
    Q_UNUSED(localUidIndex.replace(tagItemIt, item))

    updateItemColumnInView(item, Columns::NumNotesTargeted);
}

QVariant FavoritesModel::dataImpl(const int row, const Columns::type column) const
{
    if (Q_UNLIKELY((row < 0) || (row >= static_cast<int>(m_data.size())))) {
        return QVariant();
    }

    const FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();
    const FavoritesModelItem & item = rowIndex[static_cast<size_t>(row)];

    switch(column)
    {
    case Columns::Type:
        return item.type();
    case Columns::DisplayName:
        return item.displayName();
    case Columns::NumNotesTargeted:
        return item.numNotesTargeted();
    default:
        return QVariant();
    }
}

QVariant FavoritesModel::dataAccessibleText(const int row, const Columns::type column) const
{
    if (Q_UNLIKELY((row < 0) || (row >= static_cast<int>(m_data.size())))) {
        return QVariant();
    }

    const FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();
    const FavoritesModelItem & item = rowIndex[static_cast<size_t>(row)];

    QString space = QStringLiteral(" ");
    QString colon = QStringLiteral(":");

    QString accessibleText = tr("Favorited") + space;
    switch(item.type())
    {
    case FavoritesModelItem::Type::Note:
        accessibleText += tr("note");
        break;
    case FavoritesModelItem::Type::Notebook:
        accessibleText += tr("notebook");
        break;
    case FavoritesModelItem::Type::Tag:
        accessibleText += tr("tag");
        break;
    case FavoritesModelItem::Type::SavedSearch:
        accessibleText += tr("saved search");
        break;
    default:
        return QVariant();
    }

    switch(column)
    {
    case Columns::Type:
        return accessibleText;
    case Columns::DisplayName:
        accessibleText += colon + space + item.displayName();
        break;
    case Columns::NumNotesTargeted:
        accessibleText += colon + space + tr("number of targeted notes is") + space + QString::number(item.numNotesTargeted());
        break;
    default:
        return QVariant();
    }

    return accessibleText;
}

void FavoritesModel::removeItemByLocalUid(const QString & localUid)
{
    QNTRACE(QStringLiteral("FavoritesModel::removeItemByLocalUid: local uid = ") << localUid);

    FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
    auto itemIt = localUidIndex.find(localUid);
    if (Q_UNLIKELY(itemIt == localUidIndex.end())) {
        QNDEBUG(QStringLiteral("Can't find item to remove from the favorites model"));
        return;
    }

    const FavoritesModelItem & item = *itemIt;

    switch(item.type())
    {
    case FavoritesModelItem::Type::Notebook:
        {
            auto nameIt = m_lowerCaseNotebookNames.find(item.displayName().toLower());
            if (nameIt != m_lowerCaseNotebookNames.end()) {
                Q_UNUSED(m_lowerCaseNotebookNames.erase(nameIt))
            }

            break;
        }
    case FavoritesModelItem::Type::Tag:
        {
            auto nameIt = m_lowerCaseTagNames.find(item.displayName().toLower());
            if (nameIt != m_lowerCaseTagNames.end()) {
                Q_UNUSED(m_lowerCaseTagNames.erase(nameIt))
            }

            break;
        }
    case FavoritesModelItem::Type::SavedSearch:
        {
            auto nameIt = m_lowerCaseSavedSearchNames.find(item.displayName().toLower());
            if (nameIt != m_lowerCaseSavedSearchNames.end()) {
                Q_UNUSED(m_lowerCaseSavedSearchNames.erase(nameIt))
            }

            break;
        }
    default:
        break;
    }

    FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();
    auto indexIt = m_data.project<ByIndex>(itemIt);
    if (Q_UNLIKELY(indexIt == rowIndex.end())) {
        QNWARNING(QStringLiteral("Can't determine the row index for the favorites model item to remove: ") << item);
        return;
    }

    int row = static_cast<int>(std::distance(rowIndex.begin(), indexIt));
    if (Q_UNLIKELY((row < 0) || (row >= static_cast<int>(m_data.size())))) {
        QNWARNING(QStringLiteral("Invalid row index for the favorites model item to remove: index = ") << row
                  << QStringLiteral(", item: ") << item);
        return;
    }

    Q_EMIT aboutToRemoveItems();

    beginRemoveRows(QModelIndex(), row, row);
    Q_UNUSED(localUidIndex.erase(itemIt))
    endRemoveRows();

    Q_EMIT removedItems();
}

void FavoritesModel::updateItemRowWithRespectToSorting(const FavoritesModelItem & item)
{
    if (m_sortedColumn != Columns::DisplayName) {
        // Sorting by other columns is not yet implemented
        return;
    }

    FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
    auto localUidIt = localUidIndex.find(item.localUid());
    if (Q_UNLIKELY(localUidIt == localUidIndex.end())) {
        QNWARNING(QStringLiteral("Can't update item row with respect to sorting: can't find the item within the model: ") << item);
        return;
    }

    FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();
    auto it = m_data.project<ByIndex>(localUidIt);
    if (Q_UNLIKELY(it == rowIndex.end())) {
        QNWARNING(QStringLiteral("Can't update item row with respect to sorting: can't find item's original row; item: ") << item);
        return;
    }

    int originalRow = static_cast<int>(std::distance(rowIndex.begin(), it));
    if (Q_UNLIKELY((originalRow < 0) || (originalRow >= static_cast<int>(m_data.size())))) {
        QNWARNING(QStringLiteral("Can't update item row with respect to sorting: item's original row is beyond the acceptable range: ")
                  << originalRow << QStringLiteral(", item: ") << item);
        return;
    }

    FavoritesModelItem itemCopy(item);

    beginRemoveRows(QModelIndex(), originalRow, originalRow);
    Q_UNUSED(rowIndex.erase(it))
    endRemoveRows();

    auto positionIter = std::lower_bound(rowIndex.begin(), rowIndex.end(), itemCopy,
                                         Comparator(m_sortedColumn, m_sortOrder));
    if (positionIter == rowIndex.end())
    {
        int row = static_cast<int>(rowIndex.size());
        beginInsertRows(QModelIndex(), row, row);
        rowIndex.push_back(itemCopy);
        endInsertRows();
        return;
    }

    int row = static_cast<int>(std::distance(rowIndex.begin(), positionIter));
    beginInsertRows(QModelIndex(), row, row);
    Q_UNUSED(rowIndex.insert(positionIter, itemCopy))
    endInsertRows();
}

void FavoritesModel::updateItemInLocalStorage(const FavoritesModelItem & item)
{
    switch(item.type())
    {
    case FavoritesModelItem::Type::Note:
        updateNoteInLocalStorage(item);
        break;
    case FavoritesModelItem::Type::Notebook:
        updateNotebookInLocalStorage(item);
        break;
    case FavoritesModelItem::Type::Tag:
        updateTagInLocalStorage(item);
        break;
    case FavoritesModelItem::Type::SavedSearch:
        updateSavedSearchInLocalStorage(item);
        break;
    default:
        {
            QNWARNING(QStringLiteral("Detected attempt to update favorites model item in local storage for wrong favorited model item's type: ")
                      << item);
            break;
        }
    }
}

void FavoritesModel::updateNoteInLocalStorage(const FavoritesModelItem & item)
{
    QNDEBUG(QStringLiteral("FavoritesModel::updateNoteInLocalStorage: local uid = ") << item.localUid()
            << QStringLiteral(", title = ") << item.displayName());

    const Note * pCachedNote = m_noteCache.get(item.localUid());
    if (Q_UNLIKELY(!pCachedNote))
    {
        QUuid requestId = QUuid::createUuid();
        Q_UNUSED(m_findNoteToPerformUpdateRequestIds.insert(requestId))
        Note dummy;
        dummy.setLocalUid(item.localUid());
        QNTRACE(QStringLiteral("Emitting the request to find a note: local uid = ") << item.localUid()
                << QStringLiteral(", request id = ") << requestId);
        Q_EMIT findNote(dummy, /* with resource metadata = */ true, /* with resource binary data = */ false, requestId);
        return;
    }

    Note note = *pCachedNote;

    note.setLocalUid(item.localUid());
    bool dirty = note.isDirty() || !note.hasTitle() || (note.title() != item.displayName());
    note.setDirty(dirty);
    note.setTitle(item.displayName());

    QUuid requestId = QUuid::createUuid();
    Q_UNUSED(m_updateNoteRequestIds.insert(requestId))

    // While the note is being updated in the local storage,
    // remove its stale copy from the cache
    Q_UNUSED(m_noteCache.remove(note.localUid()))

    QNTRACE(QStringLiteral("Emitting the request to update the note in local storage: id = ")
            << requestId << QStringLiteral(", note: ") << note);
    Q_EMIT updateNote(note, /* update resources = */ false, /* update tags = */ false, requestId);
}

void FavoritesModel::updateNotebookInLocalStorage(const FavoritesModelItem & item)
{
    QNDEBUG(QStringLiteral("FavoritesModel::updateNotebookInLocalStorage: local uid = ") << item.localUid()
            << QStringLiteral(", name = ") << item.displayName());

    const Notebook * pCachedNotebook = m_notebookCache.get(item.localUid());
    if (Q_UNLIKELY(!pCachedNotebook))
    {
        QUuid requestId = QUuid::createUuid();
        Q_UNUSED(m_findNotebookToPerformUpdateRequestIds.insert(requestId))
        Notebook dummy;
        dummy.setLocalUid(item.localUid());
        QNTRACE(QStringLiteral("Emitting the request to find a notebook: local uid = ") << item.localUid()
                << QStringLiteral(", request id = ") << requestId);
        Q_EMIT findNotebook(dummy, requestId);
        return;
    }

    Notebook notebook = *pCachedNotebook;

    notebook.setLocalUid(item.localUid());
    bool dirty = notebook.isDirty() || !notebook.hasName() || (notebook.name() != item.displayName());
    notebook.setDirty(dirty);
    notebook.setName(item.displayName());

    QUuid requestId = QUuid::createUuid();
    Q_UNUSED(m_updateNotebookRequestIds.insert(requestId))

    // While the notebook is being updated in the local storage,
    // remove its stale copy from the cache
    Q_UNUSED(m_notebookCache.remove(notebook.localUid()))

    QNTRACE(QStringLiteral("Emitting the request to update the notebook in local storage: id = ") << requestId
            << QStringLiteral(", notebook: ") << notebook);
    Q_EMIT updateNotebook(notebook, requestId);
}

void FavoritesModel::updateTagInLocalStorage(const FavoritesModelItem & item)
{
    QNDEBUG(QStringLiteral("FavoritesModel::updateTagInLocalStorage: local uid = ") << item.localUid()
            << QStringLiteral(", name = ") << item.displayName());

    const Tag * pCachedTag = m_tagCache.get(item.localUid());
    if (Q_UNLIKELY(!pCachedTag))
    {
        QUuid requestId = QUuid::createUuid();
        Q_UNUSED(m_findTagToPerformUpdateRequestIds.insert(requestId))
        Tag dummy;
        dummy.setLocalUid(item.localUid());
        QNTRACE(QStringLiteral("Emitting the request to find a tag: local uid = ") << item.localUid()
                << QStringLiteral(", request id = ") << requestId);
        Q_EMIT findTag(dummy, requestId);
        return;
    }

    Tag tag = *pCachedTag;

    tag.setLocalUid(item.localUid());
    bool dirty = tag.isDirty() || !tag.hasName() || (tag.name() != item.displayName());
    tag.setDirty(dirty);
    tag.setName(item.displayName());

    QUuid requestId = QUuid::createUuid();
    Q_UNUSED(m_updateTagRequestIds.insert(requestId))

    // While the tag is being updated in the local storage,
    // remove its stale copy from the cache
    Q_UNUSED(m_tagCache.remove(tag.localUid()))

    QNTRACE(QStringLiteral("Emitting the request to update the tag in local storage: id = ") << requestId
            << QStringLiteral(", tag: ") << tag);
    Q_EMIT updateTag(tag, requestId);
}

void FavoritesModel::updateSavedSearchInLocalStorage(const FavoritesModelItem & item)
{
    QNDEBUG(QStringLiteral("FavoritesModel::updateSavedSearchInLocalStorage: local uid = ") << item.localUid()
            << QStringLiteral(", display name = ") << item.displayName());

    const SavedSearch * pCachedSearch = m_savedSearchCache.get(item.localUid());
    if (Q_UNLIKELY(!pCachedSearch))
    {
        QUuid requestId = QUuid::createUuid();
        Q_UNUSED(m_findSavedSearchToPerformUpdateRequestIds.insert(requestId))
        SavedSearch dummy;
        dummy.setLocalUid(item.localUid());
        QNTRACE(QStringLiteral("Emitting the request to find a saved search: local uid = ") << item.localUid()
                << QStringLiteral(", request id = ") << requestId);
        Q_EMIT findSavedSearch(dummy, requestId);
        return;
    }

    SavedSearch search = *pCachedSearch;

    search.setLocalUid(item.localUid());
    bool dirty = search.isDirty() || !search.hasName() || (search.name() != item.displayName());
    search.setDirty(dirty);
    search.setName(item.displayName());

    QUuid requestId = QUuid::createUuid();
    Q_UNUSED(m_updateSavedSearchRequestIds.insert(requestId))

    // While the saved search is being updated in the local storage,
    // remove its stale copy from the cache
    Q_UNUSED(m_savedSearchCache.remove(search.localUid()))

    QNTRACE(QStringLiteral("Emitting the request to update the saved search in local storage: id = ")
            << requestId << QStringLiteral(", saved search: ") << search);
    Q_EMIT updateSavedSearch(search, requestId);
}

bool FavoritesModel::canUpdateNote(const QString & localUid) const
{
    auto notebookLocalUidIt = m_notebookLocalUidByNoteLocalUid.find(localUid);
    if (notebookLocalUidIt == m_notebookLocalUidByNoteLocalUid.end()) {
        return false;
    }

    auto notebookGuidIt = m_notebookLocalUidToGuid.find(notebookLocalUidIt.value());
    if (notebookGuidIt == m_notebookLocalUidToGuid.end()) {
        // NOTE: this must be the local, non-synchronizable notebook as it doesn't have the Evernote service's guid;
        return true;
    }

    auto notebookRestrictionsDataIt = m_notebookRestrictionsData.find(notebookGuidIt.value());
    if (notebookRestrictionsDataIt == m_notebookRestrictionsData.end()) {
        return true;
    }

    const NotebookRestrictionsData & restrictionsData = notebookRestrictionsDataIt.value();
    return restrictionsData.m_canUpdateNotes;
}

bool FavoritesModel::canUpdateNotebook(const QString & localUid) const
{
    auto notebookGuidIt = m_notebookLocalUidToGuid.find(localUid);
    if (notebookGuidIt == m_notebookLocalUidToGuid.end()) {
        // NOTE: this must be the local, non-synchronizable notebook as it doesn't have the Evernote service's guid;
        return true;
    }

    auto notebookRestrictionsDataIt = m_notebookRestrictionsData.find(notebookGuidIt.value());
    if (notebookRestrictionsDataIt == m_notebookRestrictionsData.end()) {
        return true;
    }

    const NotebookRestrictionsData & restrictionsData = notebookRestrictionsDataIt.value();
    return restrictionsData.m_canUpdateNotebook;
}

bool FavoritesModel::canUpdateTag(const QString & localUid) const
{
    auto notebookGuidIt = m_tagLocalUidToLinkedNotebookGuid.find(localUid);
    if (notebookGuidIt == m_tagLocalUidToLinkedNotebookGuid.end()) {
        return true;
    }

    auto notebookRestrictionsDataIt = m_notebookRestrictionsData.find(notebookGuidIt.value());
    if (notebookRestrictionsDataIt == m_notebookRestrictionsData.end()) {
        return true;
    }

    const NotebookRestrictionsData & restrictionsData = notebookRestrictionsDataIt.value();
    return restrictionsData.m_canUpdateTags;
}

void FavoritesModel::unfavoriteNote(const QString & localUid)
{
    QNDEBUG(QStringLiteral("FavoritesModel::unfavoriteNote: local uid = ") << localUid);

    const Note * pCachedNote = m_noteCache.get(localUid);
    if (Q_UNLIKELY(!pCachedNote))
    {
        QUuid requestId = QUuid::createUuid();
        Q_UNUSED(m_findNoteToUnfavoriteRequestIds.insert(requestId))
        Note dummy;
        dummy.setLocalUid(localUid);
        QNTRACE(QStringLiteral("Emitting the request to find a note: local uid = ") << localUid
                << QStringLiteral(", request id = ") << requestId);
        Q_EMIT findNote(dummy, /* with resource metadata = */ true, /* with resource binary data = */ false, requestId);
        return;
    }

    Note note = *pCachedNote;

    note.setLocalUid(localUid);
    bool dirty = note.isDirty() || note.isFavorited();
    note.setDirty(dirty);
    note.setFavorited(false);

    QUuid requestId = QUuid::createUuid();
    Q_UNUSED(m_updateNoteRequestIds.insert(requestId))

    // While the note is being updated in the local storage,
    // remove its stale copy from the cache
    Q_UNUSED(m_noteCache.remove(note.localUid()))

    QNTRACE(QStringLiteral("Emitting the request to update the note in local storage: id = ") << requestId
            << QStringLiteral(", note: ") << note);
    Q_EMIT updateNote(note, /* update resources = */ false, /* update tags = */ false, requestId);
}

void FavoritesModel::unfavoriteNotebook(const QString & localUid)
{
    QNDEBUG(QStringLiteral("FavoritesModel::unfavoriteNotebook: local uid = ") << localUid);

    const Notebook * pCachedNotebook = m_notebookCache.get(localUid);
    if (Q_UNLIKELY(!pCachedNotebook))
    {
        QUuid requestId = QUuid::createUuid();
        Q_UNUSED(m_findNotebookToUnfavoriteRequestIds.insert(requestId))
        Notebook dummy;
        dummy.setLocalUid(localUid);
        QNTRACE(QStringLiteral("Emitting the request to find a notebook: local uid = ") << localUid
                << QStringLiteral(", request id = ") << requestId);
        Q_EMIT findNotebook(dummy, requestId);
        return;
    }

    Notebook notebook = *pCachedNotebook;

    notebook.setLocalUid(localUid);
    bool dirty = notebook.isDirty() || notebook.isFavorited();
    notebook.setDirty(dirty);
    notebook.setFavorited(false);

    QUuid requestId = QUuid::createUuid();
    Q_UNUSED(m_updateNotebookRequestIds.insert(requestId))

    // While the notebook is being updated in the local storage,
    // remove its stale copy from the cache
    Q_UNUSED(m_notebookCache.remove(notebook.localUid()))

    QNTRACE(QStringLiteral("Emitting the request to update the notebook in local storage: id = ") << requestId
            << QStringLiteral(", notebook: ") << notebook);
    Q_EMIT updateNotebook(notebook, requestId);
}

void FavoritesModel::unfavoriteTag(const QString & localUid)
{
    QNDEBUG(QStringLiteral("FavoritesModel::unfavoriteTag: local uid = ") << localUid);

    const Tag * pCachedTag = m_tagCache.get(localUid);
    if (Q_UNLIKELY(!pCachedTag))
    {
        QUuid requestId = QUuid::createUuid();
        Q_UNUSED(m_findTagToUnfavoriteRequestIds.insert(requestId))
        Tag dummy;
        dummy.setLocalUid(localUid);
        QNTRACE(QStringLiteral("Emitting the request to find a tag: local uid = ") << localUid
                << QStringLiteral(", request id = ") << requestId);
        Q_EMIT findTag(dummy, requestId);
        return;
    }

    Tag tag = *pCachedTag;

    tag.setLocalUid(localUid);
    bool dirty = tag.isDirty() || tag.isFavorited();
    tag.setDirty(dirty);
    tag.setFavorited(false);

    QUuid requestId = QUuid::createUuid();
    Q_UNUSED(m_updateTagRequestIds.insert(requestId))

    // While the tag is being updated in the local storage,
    // remove its stale copy from the cache
    Q_UNUSED(m_tagCache.remove(tag.localUid()))

    QNTRACE(QStringLiteral("Emitting the request to update the tag in local storage: id = ") << requestId
            << QStringLiteral(", tag: ") << tag);
    Q_EMIT updateTag(tag, requestId);
}

void FavoritesModel::unfavoriteSavedSearch(const QString & localUid)
{
    QNDEBUG(QStringLiteral("FavoritesModel::unfavoriteSavedSearch: local uid = ") << localUid);

    const SavedSearch * pCachedSearch = m_savedSearchCache.get(localUid);
    if (Q_UNLIKELY(!pCachedSearch))
    {
        QUuid requestId = QUuid::createUuid();
        Q_UNUSED(m_findSavedSearchToUnfavoriteRequestIds.insert(requestId))
        SavedSearch dummy;
        dummy.setLocalUid(localUid);
        QNTRACE(QStringLiteral("Emitting the request to find a saved search: local uid = ") << localUid
                << QStringLiteral(", request id = ") << requestId);
        Q_EMIT findSavedSearch(dummy, requestId);
        return;
    }

    SavedSearch search = *pCachedSearch;

    search.setLocalUid(localUid);
    bool dirty = search.isDirty() || search.isFavorited();
    search.setDirty(dirty);
    search.setFavorited(false);

    QUuid requestId = QUuid::createUuid();
    Q_UNUSED(m_updateSavedSearchRequestIds.insert(requestId))

    // While the saved search is being updated in the local storage,
    // remove its stale copy from the cache
    Q_UNUSED(m_savedSearchCache.remove(search.localUid()))

    QNTRACE(QStringLiteral("Emitting the request to update the saved search in local storage: id = ") << requestId
            << QStringLiteral(", saved search: ") << search);
    Q_EMIT updateSavedSearch(search, requestId);
}

void FavoritesModel::onNoteAddedOrUpdated(const Note & note, const bool tagsUpdated)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onNoteAddedOrUpdated: note local uid = ") << note.localUid()
            << QStringLiteral(", tags updated = ") << (tagsUpdated ? QStringLiteral("true") : QStringLiteral("false")));

    if (tagsUpdated) {
        m_noteCache.put(note.localUid(), note);
        checkTagsUpdateForNote(note);
    }

    if (!note.hasNotebookLocalUid()) {
        QNWARNING(QStringLiteral("Skipping the note not having the notebook local uid: ") << note);
        return;
    }

    checkNotebookUpdateForNote(note.localUid(), note.notebookLocalUid());

    if (!note.isFavorited()) {
        removeItemByLocalUid(note.localUid());
        return;
    }

    FavoritesModelItem item;
    item.setType(FavoritesModelItem::Type::Note);
    item.setLocalUid(note.localUid());
    item.setNumNotesTargeted(0);

    if (note.hasTitle())
    {
        item.setDisplayName(note.title());
    }
    else if (note.hasContent())
    {
        QString plainText = note.plainText();
        plainText.truncate(160);
        item.setDisplayName(plainText);
        // NOTE: using the text preview in this way means updating the favorites item's display name would actually create the title for the note
    }

    m_notebookLocalUidByNoteLocalUid[note.localUid()] = note.notebookLocalUid();

    FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();
    FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
    auto itemIt = localUidIndex.find(note.localUid());
    if (itemIt == localUidIndex.end())
    {
        QNDEBUG(QStringLiteral("Detected newly favorited note"));

        Q_EMIT aboutToAddItem();

        int row = static_cast<int>(rowIndex.size());
        beginInsertRows(QModelIndex(), row, row);
        rowIndex.push_back(item);
        endInsertRows();

        updateItemRowWithRespectToSorting(item);

        QModelIndex addedNoteIndex = indexForLocalUid(item.localUid());
        Q_EMIT addedItem(addedNoteIndex);

        return;
    }

    QNDEBUG(QStringLiteral("Updating the already favorited item"));

    auto indexIt = m_data.project<ByIndex>(itemIt);
    if (Q_UNLIKELY(indexIt == rowIndex.end())) {
        ErrorString error(QT_TR_NOOP("Internal error: can't project the local uid index iterator "
                                     "to the random access index iterator within the favorites model"));
        QNWARNING(error << QStringLiteral(", favorites model item: ") << item);
        Q_EMIT notifyError(error);
        return;
    }

    int row = static_cast<int>(std::distance(rowIndex.begin(), indexIt));

    Q_UNUSED(localUidIndex.replace(itemIt, item));

    QModelIndex modelIndex = createIndex(row, Columns::DisplayName);
    Q_EMIT aboutToUpdateItem(modelIndex);

    Q_EMIT dataChanged(modelIndex, modelIndex);

    updateItemRowWithRespectToSorting(item);

    modelIndex = indexForLocalUid(item.localUid());
    Q_EMIT updatedItem(modelIndex);
}

void FavoritesModel::onNotebookAddedOrUpdated(const Notebook & notebook)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onNotebookAddedOrUpdated: local uid = ") << notebook.localUid());

    m_notebookCache.put(notebook.localUid(), notebook);

    FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
    auto itemIt = localUidIndex.find(notebook.localUid());

    if (itemIt != localUidIndex.end())
    {
        const FavoritesModelItem & originalItem = *itemIt;
        auto nameIt = m_lowerCaseNotebookNames.find(originalItem.displayName().toLower());
        if (nameIt != m_lowerCaseNotebookNames.end()) {
            Q_UNUSED(m_lowerCaseNotebookNames.erase(nameIt))
        }
    }

    if (notebook.hasName()) {
        Q_UNUSED(m_lowerCaseNotebookNames.insert(notebook.name().toLower()))
    }

    if (notebook.hasGuid())
    {
        NotebookRestrictionsData & notebookRestrictionsData = m_notebookRestrictionsData[notebook.guid()];

        if (notebook.hasRestrictions())
        {
            const qevercloud::NotebookRestrictions & restrictions = notebook.restrictions();

            notebookRestrictionsData.m_canUpdateNotebook = !restrictions.noUpdateNotebook.isSet() || !restrictions.noUpdateNotebook.ref();
            notebookRestrictionsData.m_canUpdateNotes = !restrictions.noUpdateNotes.isSet() || !restrictions.noUpdateNotes.ref();
            notebookRestrictionsData.m_canUpdateTags = !restrictions.noUpdateTags.isSet() || !restrictions.noUpdateTags.ref();
        }
        else
        {
            notebookRestrictionsData.m_canUpdateNotebook = true;
            notebookRestrictionsData.m_canUpdateNotes = true;
            notebookRestrictionsData.m_canUpdateTags = true;
        }

        QNTRACE(QStringLiteral("Updated restrictions data for notebook ") << notebook.localUid() << QStringLiteral(", name ")
                << (notebook.hasName() ? QStringLiteral("\"") + notebook.name() + QStringLiteral("\"") : QStringLiteral("<not set>"))
                << QStringLiteral(", guid = ") << notebook.guid() << QStringLiteral(": can update notebook = ")
                << (notebookRestrictionsData.m_canUpdateNotebook ? QStringLiteral("true") : QStringLiteral("false"))
                << QStringLiteral(", can update notes = ")
                << (notebookRestrictionsData.m_canUpdateNotes ? QStringLiteral("true") : QStringLiteral("false"))
                << QStringLiteral(", can update tags = ")
                << (notebookRestrictionsData.m_canUpdateTags ? QStringLiteral("true") : QStringLiteral("false")));
    }

    if (!notebook.hasName()) {
        QNTRACE(QStringLiteral("Removing/skipping the notebook without a name"));
        removeItemByLocalUid(notebook.localUid());
        return;
    }

    if (!notebook.isFavorited()) {
        QNTRACE(QStringLiteral("Removing/skipping non-favorited notebook"));
        removeItemByLocalUid(notebook.localUid());
        return;
    }

    FavoritesModelItem item;
    item.setType(FavoritesModelItem::Type::Notebook);
    item.setLocalUid(notebook.localUid());
    item.setNumNotesTargeted(-1);   // That means, not known yet
    item.setDisplayName(notebook.name());

    FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();

    if (itemIt == localUidIndex.end())
    {
        QNDEBUG(QStringLiteral("Detected newly favorited notebook"));

        Q_EMIT aboutToAddItem();

        int row = static_cast<int>(rowIndex.size());
        beginInsertRows(QModelIndex(), row, row);
        rowIndex.push_back(item);
        endInsertRows();

        updateItemRowWithRespectToSorting(item);

        QModelIndex addedNotebookIndex = indexForLocalUid(item.localUid());
        Q_EMIT addedItem(addedNotebookIndex);

        // Need to figure out how many notes this notebook targets
        requestNoteCountForNotebook(notebook.localUid(), NoteCountRequestOption::IfNotAlreadyRunning);

        return;
    }

    QNDEBUG(QStringLiteral("Updating the already favorited notebook item"));

    const FavoritesModelItem & originalItem = *itemIt;
    item.setNumNotesTargeted(originalItem.numNotesTargeted());

    auto indexIt = m_data.project<ByIndex>(itemIt);
    if (Q_UNLIKELY(indexIt == rowIndex.end())) {
        ErrorString error(QT_TR_NOOP("Internal error: can't project the local uid index iterator "
                                     "to the random access index iterator within the favorites model"));
        QNWARNING(error << QStringLiteral(", favorites model item: ") << item);
        Q_EMIT notifyError(error);
        return;
    }

    int row = static_cast<int>(std::distance(rowIndex.begin(), indexIt));
    Q_UNUSED(localUidIndex.replace(itemIt, item))

    QModelIndex modelIndex = createIndex(row, Columns::DisplayName);
    Q_EMIT aboutToUpdateItem(modelIndex);

    Q_EMIT dataChanged(modelIndex, modelIndex);

    updateItemRowWithRespectToSorting(item);

    modelIndex = indexForLocalUid(item.localUid());
    Q_EMIT updatedItem(modelIndex);
}

void FavoritesModel::onTagAddedOrUpdated(const Tag & tag)
{
    QNTRACE(QStringLiteral("FavoritesModel::onTagAddedOrUpdated: local uid = ") << tag.localUid());

    m_tagCache.put(tag.localUid(), tag);

    FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
    auto itemIt = localUidIndex.find(tag.localUid());

    if (itemIt != localUidIndex.end())
    {
        const FavoritesModelItem & originalItem = *itemIt;
        auto nameIt = m_lowerCaseTagNames.find(originalItem.displayName().toLower());
        if (nameIt != m_lowerCaseTagNames.end()) {
            Q_UNUSED(m_lowerCaseTagNames.erase(nameIt))
        }
    }

    if (tag.hasName()) {
        Q_UNUSED(m_lowerCaseTagNames.insert(tag.name().toLower()))
    }
    else {
        QNTRACE(QStringLiteral("Removing/skipping the tag without a name"));
        removeItemByLocalUid(tag.localUid());
        return;
    }

    if (!tag.isFavorited()) {
        QNTRACE(QStringLiteral("Removing/skipping non-favorited tag"));
        removeItemByLocalUid(tag.localUid());
        return;
    }

    FavoritesModelItem item;
    item.setType(FavoritesModelItem::Type::Tag);
    item.setLocalUid(tag.localUid());
    item.setNumNotesTargeted(-1);   // That means, not known yet
    item.setDisplayName(tag.name());

    FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();

    if (itemIt == localUidIndex.end())
    {
        QNDEBUG(QStringLiteral("Detected newly favorited tag"));

        Q_EMIT aboutToAddItem();

        int row = static_cast<int>(rowIndex.size());
        beginInsertRows(QModelIndex(), row, row);
        rowIndex.push_back(item);
        endInsertRows();

        updateItemRowWithRespectToSorting(item);

        QModelIndex addedTagIndex = indexForLocalUid(item.localUid());
        Q_EMIT addedItem(addedTagIndex);

        // Need to figure out how many notes this tag targets
        requestNoteCountForTag(tag.localUid(), NoteCountRequestOption::IfNotAlreadyRunning);

        return;
    }

    QNDEBUG(QStringLiteral("Updating the already favorited tag item"));

    const FavoritesModelItem & originalItem = *itemIt;
    item.setNumNotesTargeted(originalItem.numNotesTargeted());

    auto indexIt = m_data.project<ByIndex>(itemIt);
    if (Q_UNLIKELY(indexIt == rowIndex.end())) {
        ErrorString error(QT_TR_NOOP("Internal error: can't project the local uid index iterator "
                                     "to the random access index iterator within the favorites model"));
        QNWARNING(error << QStringLiteral(", favorites model item: ") << item);
        Q_EMIT notifyError(error);
        return;
    }

    int row = static_cast<int>(std::distance(rowIndex.begin(), indexIt));
    Q_UNUSED(localUidIndex.replace(itemIt, item))

    QModelIndex modelIndex = createIndex(row, Columns::DisplayName);
    Q_EMIT aboutToUpdateItem(modelIndex);

    Q_EMIT dataChanged(modelIndex, modelIndex);

    updateItemRowWithRespectToSorting(item);

    modelIndex = indexForLocalUid(item.localUid());
    Q_EMIT updatedItem(modelIndex);
}

void FavoritesModel::onSavedSearchAddedOrUpdated(const SavedSearch & search)
{
    QNDEBUG(QStringLiteral("FavoritesModel::onSavedSearchAddedOrUpdated: local uid = ") << search.localUid());

    m_savedSearchCache.put(search.localUid(), search);

    FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();
    auto itemIt = localUidIndex.find(search.localUid());

    if (itemIt != localUidIndex.end())
    {
        const FavoritesModelItem & originalItem = *itemIt;
        auto nameIt = m_lowerCaseSavedSearchNames.find(originalItem.displayName().toLower());
        if (nameIt != m_lowerCaseSavedSearchNames.end()) {
            Q_UNUSED(m_lowerCaseSavedSearchNames.erase(nameIt))
        }
    }

    if (search.hasName()) {
        Q_UNUSED(m_lowerCaseSavedSearchNames.insert(search.name().toLower()))
    }
    else {
        QNTRACE(QStringLiteral("Removing/skipping the search without a name"));
        removeItemByLocalUid(search.localUid());
        return;
    }

    if (!search.isFavorited()) {
        QNTRACE(QStringLiteral("Removing/skipping non-favorited search"));
        removeItemByLocalUid(search.localUid());
        return;
    }

    FavoritesModelItem item;
    item.setType(FavoritesModelItem::Type::SavedSearch);
    item.setLocalUid(search.localUid());
    item.setNumNotesTargeted(-1);
    item.setDisplayName(search.name());

    FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();

    if (itemIt == localUidIndex.end())
    {
        QNDEBUG(QStringLiteral("Detected newly favorited saved search"));

        Q_EMIT aboutToAddItem();

        int row = static_cast<int>(rowIndex.size());
        beginInsertRows(QModelIndex(), row, row);
        rowIndex.push_back(item);
        endInsertRows();

        updateItemRowWithRespectToSorting(item);

        QModelIndex addedSavedSearchIndex = indexForLocalUid(item.localUid());
        Q_EMIT addedItem(addedSavedSearchIndex);

        return;
    }

    QNDEBUG(QStringLiteral("Updating the already favorited saved search item"));

    auto indexIt = m_data.project<ByIndex>(itemIt);
    if (Q_UNLIKELY(indexIt == rowIndex.end())) {
        ErrorString error(QT_TR_NOOP("Internal error: can't project the local uid index iterator "
                                     "to the random access index iterator within the favorites model"));
        QNWARNING(error << QStringLiteral(", favorites model item: ") << item);
        Q_EMIT notifyError(error);
        return;
    }

    int row = static_cast<int>(std::distance(rowIndex.begin(), indexIt));

    localUidIndex.replace(itemIt, item);
    QModelIndex modelIndex = createIndex(row, Columns::DisplayName);
    Q_EMIT aboutToUpdateItem(modelIndex);

    Q_EMIT dataChanged(modelIndex, modelIndex);

    updateItemRowWithRespectToSorting(item);

    modelIndex = indexForLocalUid(item.localUid());
    Q_EMIT updatedItem(modelIndex);
}

void FavoritesModel::checkNotebookUpdateForNote(const QString & noteLocalUid, const QString & notebookLocalUid)
{
    QNDEBUG(QStringLiteral("FavoritesModel::checkNotebookUpdateForNote: note local uid = ") << noteLocalUid
            << QStringLiteral(", notebook local uid = ") << notebookLocalUid);

    if (!m_receivedNotebookLocalUidsForAllNotes) {
        QNDEBUG(QStringLiteral("Notebook local uid hasn't been received for all notes yet"));
        requestNoteCountForAllNotebooks(NoteCountRequestOption::Force);
        return;
    }

    auto notebookLocalUidIt = m_notebookLocalUidByNoteLocalUid.find(noteLocalUid);
    if (notebookLocalUidIt == m_notebookLocalUidByNoteLocalUid.end())
    {
        QNDEBUG(QStringLiteral("Haven't found the previous notebook local uid for this note"));

        m_notebookLocalUidByNoteLocalUid[noteLocalUid] = notebookLocalUid;

        // Since it's unclear whether the note count was changed for this and/or previous notebook,
        // need to request the note count for all notebooks
        requestNoteCountForAllNotebooks(NoteCountRequestOption::Force);

        return;
    }

    QString previousNotebookLocalUid = notebookLocalUidIt.value();
    if (previousNotebookLocalUid == notebookLocalUid) {
        QNDEBUG(QStringLiteral("The notebook hasn't changed for this note"));
        return;
    }

    QNDEBUG(QStringLiteral("Detected the update of notebook local uid for note ") << noteLocalUid
            << QStringLiteral(": was ") << previousNotebookLocalUid << QStringLiteral(", became ") << notebookLocalUid);

    notebookLocalUidIt.value() = notebookLocalUid;

    checkAndDecrementNoteCountPerNotebook(previousNotebookLocalUid);
    checkAndIncrementNoteCountPerNotebook(notebookLocalUid);
}

void FavoritesModel::checkAndUpdateNoteCountPerNotebookAfterNoteExpunge(const Note & note)
{
    QNDEBUG(QStringLiteral("FavoritesModel::checkAndUpdateNoteCountPerNotebookAfterNoteExpunge: note local uid = ")
            << note.localUid());

    auto notebookLocalUidIt = m_notebookLocalUidByNoteLocalUid.find(note.localUid());
    if (notebookLocalUidIt == m_notebookLocalUidByNoteLocalUid.end())
    {
        QNDEBUG(QStringLiteral("Haven't found the notebook local uid for the expunged note"));

        // Since it's unclear whether some notebook within the favorites model was affected,
        // need to check and re-subscribe to the note counts for all notebooks
        requestNoteCountForAllNotebooks(NoteCountRequestOption::Force);
        return;
    }

    const QString & notebookLocalUid = notebookLocalUidIt.value();
    checkAndDecrementNoteCountPerNotebook(notebookLocalUid);
    Q_UNUSED(m_notebookLocalUidByNoteLocalUid.erase(notebookLocalUidIt))
}

void FavoritesModel::checkTagsUpdateForNote(const Note & note)
{
    QNDEBUG(QStringLiteral("FavoritesModel::checkTagsUpdateForNote: note local uid = ") << note.localUid());

    if (!m_receivedTagLocalUidsForAllNotes) {
        QNDEBUG(QStringLiteral("Tag local uids were not received for all tags yet"));
        requestNoteCountForAllTags(NoteCountRequestOption::Force);
        return;
    }

    QStringList tagLocalUids;
    if (note.hasTagLocalUids()) {
        tagLocalUids = note.tagLocalUids();
    }

    auto tagLocalUidsIt = m_tagLocalUidsByNoteLocalUid.find(note.localUid());
    if (tagLocalUidsIt == m_tagLocalUidsByNoteLocalUid.end())
    {
        QNDEBUG(QStringLiteral("Haven't found any previous tag local uids for this note"));

        if (!tagLocalUids.isEmpty()) {
            m_tagLocalUidsByNoteLocalUid[note.localUid()] = tagLocalUids;
        }

        // Since it's unclear whether the note count was changed for any of these tags,
        // need to request the note count for all tags
        requestNoteCountForAllTags(NoteCountRequestOption::Force);

        return;
    }

    QStringList previousTagLocalUids = tagLocalUidsIt.value();

    bool sameTags = (previousTagLocalUids.size() == tagLocalUids.size());
    if (sameTags)
    {
        for(auto it = tagLocalUids.constBegin(), end = tagLocalUids.constEnd(); it != end; ++it)
        {
            if (!previousTagLocalUids.contains(*it)) {
                sameTags = false;
                break;
            }
        }
    }

    if (sameTags) {
        QNDEBUG(QStringLiteral("The note's mapping to tags hasn't changed"));
        return;
    }

    QNDEBUG(QStringLiteral("Detected the update of note's tags for note ") << note.localUid()
            << QStringLiteral(": previous tags' local uids: ") << previousTagLocalUids.join(QStringLiteral(", "))
            << QStringLiteral("; new tags' local uids: ") << tagLocalUids.join(QStringLiteral(", ")));

    tagLocalUidsIt.value() = tagLocalUids;

    tagLocalUids << previousTagLocalUids;
    Q_UNUSED(tagLocalUids.removeDuplicates())

    for(auto it = tagLocalUids.constBegin(), end = tagLocalUids.constEnd(); it != end; ++it) {
        requestNoteCountForTag(*it, NoteCountRequestOption::Force);
    }
}

void FavoritesModel::checkAndUpdateNoteCountPerTagAfterNoteExpunge(const Note & note)
{
    QNDEBUG(QStringLiteral("FavoritesModel::checkAndUpdateNoteCountPerTagAfterNoteExpunge: note local uid = ") << note.localUid());

    auto tagLocalUidsIt = m_tagLocalUidsByNoteLocalUid.find(note.localUid());
    if (tagLocalUidsIt == m_tagLocalUidsByNoteLocalUid.end()) {
        QNDEBUG(QStringLiteral("Haven't found any tag local uids for the expunged note"));
        return;
    }

    const QStringList & tagLocalUids = tagLocalUidsIt.value();
    if (tagLocalUids.isEmpty()) {
        QNDEBUG(QStringLiteral("The expunged note had no tags"));
        Q_UNUSED(m_tagLocalUidsByNoteLocalUid.erase(tagLocalUidsIt))
        return;
    }

    for(auto it = tagLocalUids.constBegin(), end = tagLocalUids.constEnd(); it != end; ++it) {
        const QString & removedTagLocalUid = *it;
        checkAndDecrementNoteCountPerTag(removedTagLocalUid);
    }

    Q_UNUSED(m_tagLocalUidsByNoteLocalUid.erase(tagLocalUidsIt))
}

void FavoritesModel::updateItemColumnInView(const FavoritesModelItem & item, const Columns::type column)
{
    QNDEBUG(QStringLiteral("FavoritesModel::updateItemColumnInView: item = ") << item << QStringLiteral("\nColumn = ") << column);

    const FavoritesDataByIndex & rowIndex = m_data.get<ByIndex>();
    const FavoritesDataByLocalUid & localUidIndex = m_data.get<ByLocalUid>();

    auto itemIt = localUidIndex.find(item.localUid());
    if (itemIt == localUidIndex.end()) {
        QNDEBUG(QStringLiteral("Can't find item by local uid"));
        return;
    }

    if (m_sortedColumn != column)
    {
        auto itemIndexIt = m_data.project<ByIndex>(itemIt);
        if (Q_LIKELY(itemIndexIt != rowIndex.end())) {
            int row = static_cast<int>(std::distance(rowIndex.begin(), itemIndexIt));
            QModelIndex modelIndex = createIndex(row, column);
            QNTRACE(QStringLiteral("Emitting dataChanged signal for row ") << row
                    << QStringLiteral(" and column ") << column);
            Q_EMIT dataChanged(modelIndex, modelIndex);
            return;
        }

        ErrorString error(QT_TR_NOOP("Internal error: can't project the local uid index iterator "
                                     "to the random access index iterator within the favorites model"));
        QNWARNING(error << QStringLiteral(", favorites model item: ") << item);
        Q_EMIT notifyError(error);
    }

    updateItemRowWithRespectToSorting(item);
}

void FavoritesModel::checkAllItemsListed()
{
    if (m_allItemsListed) {
        return;
    }

    if (m_listNotesRequestId.isNull() && m_listNotebooksRequestId.isNull() && m_listTagsRequestId.isNull() && m_listSavedSearchesRequestId.isNull()) {
        QNDEBUG(QStringLiteral("Listed all favorites model's items"));
        m_allItemsListed = true;
        Q_EMIT notifyAllItemsListed();
    }
}

void FavoritesModel::buildTagLocalUidsByNoteLocalUidsHash(const NoteModel & noteModel)
{
    QNDEBUG(QStringLiteral("FavoritesModel::buildTagLocalUidsByNoteLocalUidsHash"));

    m_tagLocalUidsByNoteLocalUid.clear();

    int numNotes = noteModel.rowCount(QModelIndex());
    for(int i = 0; i < numNotes; ++i)
    {
        const NoteModelItem * pNoteModelItem = noteModel.itemAtRow(i);
        if (Q_UNLIKELY(!pNoteModelItem)) {
            QNWARNING(QStringLiteral("Can't find note model item at row ") << i << QStringLiteral(" even though there are ")
                      << numNotes << QStringLiteral(" rows within the model"));
            continue;
        }

        const QStringList & tagLocalUids = pNoteModelItem->tagLocalUids();
        if (tagLocalUids.isEmpty()) {
            QNTRACE(QStringLiteral("Note ") << pNoteModelItem->localUid() << QStringLiteral(" has no tags"));
            continue;
        }

        QNTRACE(QStringLiteral("Tag local uids for note local uid ") << pNoteModelItem->localUid()
                << QStringLiteral(": ") << tagLocalUids.join(QStringLiteral(", ")));

        m_tagLocalUidsByNoteLocalUid[pNoteModelItem->localUid()] = tagLocalUids;
    }

    m_receivedTagLocalUidsForAllNotes = true;
}

void FavoritesModel::buildNotebookLocalUidByNoteLocalUidsHash(const NoteModel & noteModel)
{
    QNDEBUG(QStringLiteral("FavoritesModel::buildNotebookLocalUidByNoteLocalUidsHash"));

    m_notebookLocalUidByNoteLocalUid.clear();

    int numNotes = noteModel.rowCount(QModelIndex());
    for(int i = 0; i < numNotes; ++i)
    {
        const NoteModelItem * pNoteModelItem = noteModel.itemAtRow(i);
        if (Q_UNLIKELY(!pNoteModelItem)) {
            QNWARNING(QStringLiteral("Can't find note model item at row ") << i << QStringLiteral(" even though there are ")
                      << numNotes << QStringLiteral(" rows within the model"));
            continue;
        }

        const QString & notebookLocalUid = pNoteModelItem->notebookLocalUid();
        if (Q_UNLIKELY(notebookLocalUid.isEmpty())) {
            QNWARNING(QStringLiteral("Found note model item without notebook local uid: ") << *pNoteModelItem);
            continue;
        }

        QNTRACE(QStringLiteral("Notebook local uid for note local uid ") << pNoteModelItem->localUid()
                << QStringLiteral(": ") << notebookLocalUid);

        m_notebookLocalUidByNoteLocalUid[pNoteModelItem->localUid()] = notebookLocalUid;
    }

    m_receivedNotebookLocalUidsForAllNotes = true;
}

bool FavoritesModel::Comparator::operator()(const FavoritesModelItem & lhs, const FavoritesModelItem & rhs) const
{
    bool less = false;
    bool greater = false;

    switch(m_sortedColumn)
    {
    case Columns::DisplayName:
        {
            int compareResult = lhs.displayName().localeAwareCompare(rhs.displayName());
            less = compareResult < 0;
            greater = compareResult > 0;
            break;
        }
    case Columns::Type:
        {
            less = lhs.type() < rhs.type();
            greater = lhs.type() > rhs.type();
            break;
        }
    case Columns::NumNotesTargeted:
        {
            less = lhs.numNotesTargeted() < rhs.numNotesTargeted();
            greater = lhs.numNotesTargeted() > rhs.numNotesTargeted();
            break;
        }
    default:
        return false;
    }

    if (m_sortOrder == Qt::AscendingOrder) {
        return less;
    }
    else {
        return greater;
    }
}

} // namespace quentier

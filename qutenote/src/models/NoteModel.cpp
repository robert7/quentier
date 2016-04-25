#include "NoteModel.h"

namespace qute_note {

NoteModel::NoteModel(LocalStorageManagerThreadWorker & localStorageManagerThreadWorker,
                     QObject * parent) :
    QAbstractItemModel(parent),
    m_data(),
    m_listNotesOffset(0),
    m_listNotesRequestId(),
    m_noteItemsNotYetInLocalStorageUids(),
    m_cache(),
    m_addNoteRequestIds(),
    m_updateNoteRequestIds(),
    m_deleteNoteRequestIds(),
    m_expungeNoteRequestIds(),
    m_findNoteToRestoreFailedUpdateRequestIds(),
    m_findNoteToPerformUpdateRequestIds(),
    m_sortedColumn(Columns::ModificationTimestamp),
    m_sortOrder(Qt::AscendingOrder)
{
    createConnections(localStorageManagerThreadWorker);
    requestNoteList();
}

NoteModel::~NoteModel()
{}

QModelIndex NoteModel::indexForLocalUid(const QString & localUid) const
{
    // TODO: implement
    Q_UNUSED(localUid)
    return QModelIndex();
}

Qt::ItemFlags NoteModel::flags(const QModelIndex & index) const
{
    // TODO: implement
    return QAbstractItemModel::flags(index);
}

QVariant NoteModel::data(const QModelIndex & index, int role) const
{
    // TODO: implement
    Q_UNUSED(index)
    Q_UNUSED(role)
    return QVariant();
}

QVariant NoteModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    // TODO: implement
    Q_UNUSED(section)
    Q_UNUSED(orientation)
    Q_UNUSED(role)
    return QVariant();
}

int NoteModel::rowCount(const QModelIndex & parent) const
{
    // TODO: implement
    Q_UNUSED(parent)
    return 0;
}

int NoteModel::columnCount(const QModelIndex & parent) const
{
    // TODO: implement
    Q_UNUSED(parent)
    return 0;
}

QModelIndex NoteModel::index(int row, int column, const QModelIndex & parent) const
{
    // TODO: implement
    Q_UNUSED(row)
    Q_UNUSED(column)
    Q_UNUSED(parent)
    return QModelIndex();
}

QModelIndex NoteModel::parent(const QModelIndex & index) const
{
    // TODO: implement
    Q_UNUSED(index)
    return QModelIndex();
}

bool NoteModel::setHeaderData(int section, Qt::Orientation orientation, const QVariant & value, int role)
{
    Q_UNUSED(section)
    Q_UNUSED(orientation)
    Q_UNUSED(value)
    Q_UNUSED(role)
    return false;
}

bool NoteModel::setData(const QModelIndex & index, const QVariant & value, int role)
{
    // TODO: implement
    Q_UNUSED(index)
    Q_UNUSED(value)
    Q_UNUSED(role)
    return false;
}

bool NoteModel::insertRows(int row, int count, const QModelIndex & parent)
{
    // TODO: implement
    Q_UNUSED(row)
    Q_UNUSED(count)
    Q_UNUSED(parent)
    return false;
}

bool NoteModel::removeRows(int row, int count, const QModelIndex & parent)
{
    // TODO: implement
    Q_UNUSED(row)
    Q_UNUSED(count)
    Q_UNUSED(parent)
    return false;
}

void NoteModel::sort(int column, Qt::SortOrder order)
{
    // TODO: implement
    Q_UNUSED(column)
    Q_UNUSED(order)
}

void NoteModel::onAddNoteComplete(Note note, QUuid requestId)
{
    // TODO: implement
    Q_UNUSED(note)
    Q_UNUSED(requestId)
}

void NoteModel::onAddNoteFailed(Note note, QString errorDescription, QUuid requestId)
{
    // TODO: implement
    Q_UNUSED(note)
    Q_UNUSED(errorDescription)
    Q_UNUSED(requestId)
}

void NoteModel::onUpdateNoteComplete(Note note, QUuid requestId)
{
    // TODO: implement
    Q_UNUSED(note)
    Q_UNUSED(requestId)
}

void NoteModel::onUpdateNoteFailed(Note note, QString errorDescription, QUuid requestId)
{
    // TODO: implement
    Q_UNUSED(note)
    Q_UNUSED(errorDescription)
    Q_UNUSED(requestId)
}

void NoteModel::onFindNoteComplete(Note note, QUuid requestId)
{
    // TODO: implement
    Q_UNUSED(note)
    Q_UNUSED(requestId)
}

void NoteModel::onFindNoteFailed(Note note, QString errorDescription, QUuid requestId)
{
    // TODO: implement
    Q_UNUSED(note)
    Q_UNUSED(errorDescription)
    Q_UNUSED(requestId)
}

void NoteModel::onListNotesComplete(LocalStorageManager::ListObjectsOptions flag, bool withResourceBinaryData,
                                    size_t limit, size_t offset, LocalStorageManager::ListNotesOrder::type order,
                                    LocalStorageManager::OrderDirection::type orderDirection,
                                    QList<Note> foundNotes, QUuid requestId)
{
    // TODO: implement
    Q_UNUSED(flag)
    Q_UNUSED(withResourceBinaryData)
    Q_UNUSED(limit)
    Q_UNUSED(offset)
    Q_UNUSED(order)
    Q_UNUSED(orderDirection)
    Q_UNUSED(foundNotes)
    Q_UNUSED(requestId)
}

void NoteModel::onListNotesFailed(LocalStorageManager::ListObjectsOptions flag, bool withResourceBinaryData,
                                  size_t limit, size_t offset, LocalStorageManager::ListNotesOrder::type order,
                                  LocalStorageManager::OrderDirection::type orderDirection,
                                  QString errorDescription, QUuid requestId)
{
    // TODO: implement
    Q_UNUSED(flag)
    Q_UNUSED(withResourceBinaryData)
    Q_UNUSED(limit)
    Q_UNUSED(offset)
    Q_UNUSED(order)
    Q_UNUSED(orderDirection)
    Q_UNUSED(errorDescription)
    Q_UNUSED(requestId)
}

void NoteModel::onDeleteNoteComplete(Note note, QUuid requestId)
{
    // TODO: implement
    Q_UNUSED(note)
    Q_UNUSED(requestId)
}

void NoteModel::onDeleteNoteFailed(Note note, QString errorDescription, QUuid requestId)
{
    // TODO: implement
    Q_UNUSED(note)
    Q_UNUSED(errorDescription)
    Q_UNUSED(requestId)
}

void NoteModel::onExpungeNoteComplete(Note note, QUuid requestId)
{
    // TODO: implement
    Q_UNUSED(note)
    Q_UNUSED(requestId)
}

void NoteModel::onExpungeNoteFailed(Note note, QString errorDescription, QUuid requestId)
{
    // TODO: implement
    Q_UNUSED(note)
    Q_UNUSED(errorDescription)
    Q_UNUSED(requestId)
}

void NoteModel::createConnections(LocalStorageManagerThreadWorker & localStorageManagerThreadWorker)
{
    // TODO: implement
    Q_UNUSED(localStorageManagerThreadWorker)
}

void NoteModel::requestNoteList()
{
    // TODO: implement
}

QVariant NoteModel::dataText(const NoteModelItem & item, const Columns::type column) const
{
    // TODO: implement
    Q_UNUSED(item)
    Q_UNUSED(column)
    return QVariant();
}

QVariant NoteModel::dataAccessibleText(const NoteModelItem & item, const Columns::type column) const
{
    // TODO: implement
    Q_UNUSED(item)
    Q_UNUSED(column)
    return QVariant();
}

void NoteModel::removeItemByLocalUid(const QString & localUid)
{
    // TODO: implement
    Q_UNUSED(localUid)
}

void NoteModel::updateItemRowWithRespectToSorting(const NoteModelItem & item)
{
    // TODO: implement
    Q_UNUSED(item)
}

void NoteModel::updatePersistentModelIndices()
{
    // TODO: implement
}

void NoteModel::updateNoteInLocalStorage(const NoteModelItem & item)
{
    // TODO: implement
    Q_UNUSED(item)
}

} // namespace qute_note

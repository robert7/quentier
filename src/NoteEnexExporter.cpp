#include "NoteEnexExporter.h"
#include "NoteEditorTabsAndWindowsCoordinator.h"
#include "widgets/NoteEditorWidget.h"
#include "models/TagModel.h"
#include <quentier/local_storage/LocalStorageManagerThreadWorker.h>
#include <quentier/enml/ENMLConverter.h>
#include <quentier/logging/QuentierLogger.h>
#include <QVector>

#define QUENTIER_ENEX_VERSION QStringLiteral("Quentier")

namespace quentier {

NoteEnexExporter::NoteEnexExporter(LocalStorageManagerThreadWorker & localStorageWorker,
                                   NoteEditorTabsAndWindowsCoordinator & coordinator,
                                   TagModel & tagModel, QObject * parent) :
    QObject(parent),
    m_localStorageWorker(localStorageWorker),
    m_noteEditorTabsAndWindowsCoordinator(coordinator),
    m_pTagModel(&tagModel),
    m_noteLocalUids(),
    m_findNoteRequestIds(),
    m_notesByLocalUid(),
    m_includeTags(),
    m_connectedToLocalStorage(false)
{
    if (!tagModel.allTagsListed()) {
        QObject::connect(&tagModel, QNSIGNAL(TagModel,allTagsListed),
                         this, QNSLOT(NoteEnexExporter,onAllTagsListed));
    }
}

void NoteEnexExporter::setNoteLocalUids(const QStringList & noteLocalUids)
{
    QNDEBUG(QStringLiteral("NoteEnexExporter::setNoteLocalUids: ")
            << noteLocalUids.join(QStringLiteral(", ")));

    if (isInProgress()) {
        clear();
    }

    m_noteLocalUids = noteLocalUids;
}

void NoteEnexExporter::setIncludeTags(const bool includeTags)
{
    QNDEBUG(QStringLiteral("NoteEnexExporter::setIncludeTags: ")
            << (includeTags ? QStringLiteral("true") : QStringLiteral("false")));

    if (m_includeTags == includeTags) {
        QNDEBUG(QStringLiteral("The setting has not changed, won't do anything"));
        return;
    }

    if (isInProgress()) {
        clear();
    }

    m_includeTags = includeTags;
}

bool NoteEnexExporter::isInProgress() const
{
    QNDEBUG(QStringLiteral("NoteEnexExporter::isInProgress"));

    if (m_noteLocalUids.isEmpty()) {
        QNDEBUG(QStringLiteral("No note local uids are set"));
        return false;
    }

    if (m_findNoteRequestIds.isEmpty()) {
        QNDEBUG(QStringLiteral("No pending requests to find notes in the local storage"));
        return false;
    }

    return true;
}

void NoteEnexExporter::start()
{
    QNDEBUG(QStringLiteral("NoteEnexExporter::start"));

    if (m_noteLocalUids.isEmpty()) {
        ErrorString errorDescription(QT_TRANSLATE_NOOP("", "Can't export note to ENEX: "
                                                           "no note local uids were specified"));
        QNWARNING(errorDescription);
        emit failedToExportNotesToEnex(errorDescription);
        return;
    }

    if (m_includeTags && m_pTagModel.isNull()) {
        ErrorString errorDescription(QT_TRANSLATE_NOOP("", "Can't export note to ENEX: "
                                                           "the tag model has expited"));
        QNWARNING(errorDescription);
        emit failedToExportNotesToEnex(errorDescription);
        return;
    }

    m_findNoteRequestIds.clear();
    m_notesByLocalUid.clear();

    for(auto it = m_noteLocalUids.constBegin(), end = m_noteLocalUids.constEnd(); it != end; ++it)
    {
        const QString & noteLocalUid = *it;

        NoteEditorWidget * pNoteEditorWidget = m_noteEditorTabsAndWindowsCoordinator.noteEditorWidgetForNoteLocalUid(noteLocalUid);
        if (!pNoteEditorWidget) {
            QNTRACE(QStringLiteral("Found no note editor widget for note local uid ") << noteLocalUid);
            findNoteInLocalStorage(noteLocalUid);
            continue;
        }

        QNTRACE(QStringLiteral("Found note editor with loaded note ") << noteLocalUid);

        const Note * pNote = pNoteEditorWidget->currentNote();
        if (Q_UNLIKELY(!pNote)) {
            QNDEBUG(QStringLiteral("There is no note in the editor, will try to find it "
                                   "in the local storage"));
            findNoteInLocalStorage(noteLocalUid);
            continue;
        }

        if (!pNoteEditorWidget->isModified()) {
            QNTRACE(QStringLiteral("Fetched the unmodified note from editor: ") << noteLocalUid);
            m_notesByLocalUid[noteLocalUid] = *pNote;
            continue;
        }

        QNTRACE(QStringLiteral("The note within the editor was modified, saving it"));

        ErrorString noteSavingError;
        NoteEditorWidget::NoteSaveStatus::type saveStatus = pNoteEditorWidget->checkAndSaveModifiedNote(noteSavingError);
        if (saveStatus != NoteEditorWidget::NoteSaveStatus::Ok) {
            QNWARNING(QStringLiteral("Could not save the note loaded into the editor: status = ")
                      << saveStatus << QStringLiteral(", error: ") << noteSavingError
                      << QStringLiteral("; will try to find the note in the local storage"));
            findNoteInLocalStorage(noteLocalUid);
            continue;
        }

        pNote = pNoteEditorWidget->currentNote();
        if (Q_UNLIKELY(!pNote)) {
            QNWARNING(QStringLiteral("Note editor's current note has unexpectedly become nullptr "
                                     "after the note has been saved; will try to find the note "
                                     "in the local storage"));
            findNoteInLocalStorage(noteLocalUid);
            continue;
        }

        QNTRACE(QStringLiteral("Fetched the modified & saved note from editor: ") << noteLocalUid);
        m_notesByLocalUid[noteLocalUid] = *pNote;
    }

    if (!m_findNoteRequestIds.isEmpty()) {
        QNDEBUG(QStringLiteral("Not all requested notes were found loaded into the editors, "
                               "currently pending ") << m_findNoteRequestIds.size()
                << QStringLiteral(" find note in local storage requests "));
        return;
    }

    QNDEBUG(QStringLiteral("All requested notes were found loaded into the editors "
                           "and were successfully gathered from them"));

    if (m_includeTags && !m_pTagModel->allTagsListed()) {
        QNDEBUG(QStringLiteral("Waiting for the tag model to get all tags listed"));
        return;
    }

    ErrorString errorDescription;
    QString enex = convertNotesToEnex(errorDescription);
    if (enex.isEmpty()) {
        emit failedToExportNotesToEnex(errorDescription);
        return;
    }

    emit notesExportedToEnex(enex);
}

void NoteEnexExporter::clear()
{
    QNDEBUG(QStringLiteral("NoteEnexExporter::clear"));

    m_targetEnexFilePath.clear();
    m_noteLocalUids.clear();
    m_findNoteRequestIds.clear();
    m_notesByLocalUid.clear();

    disconnectFromLocalStorage();
    m_connectedToLocalStorage = false;
}

void NoteEnexExporter::onFindNoteComplete(Note note, bool withResourceBinaryData, QUuid requestId)
{
    auto it = m_findNoteRequestIds.find(requestId);
    if (it == m_findNoteRequestIds.end()) {
        return;
    }

    QNDEBUG(QStringLiteral("NoteEnexExporter::onFindNoteComplete: request id = ")
            << requestId << QStringLiteral(", note: ") << note);

    Q_UNUSED(withResourceBinaryData)

    m_notesByLocalUid[note.localUid()] = note;
    m_findNoteRequestIds.erase(it);

    if (!m_findNoteRequestIds.isEmpty()) {
        QNDEBUG(QStringLiteral("Still pending  ") << m_findNoteRequestIds.size()
                << QStringLiteral(" find note in local storage requests "));
        return;
    }

    if (m_includeTags)
    {
        if (Q_UNLIKELY(m_pTagModel.isNull())) {
            ErrorString errorDescription(QT_TRANSLATE_NOOP("", "Can't export note(s) to ENEX: "
                                                               "the tag model has expired"));
            QNWARNING(errorDescription);
            clear();
            emit failedToExportNotesToEnex(errorDescription);
            return;
        }

        if (!m_pTagModel->allTagsListed()) {
            QNDEBUG(QStringLiteral("Not all tags were listed within the tag model yet"));
            return;
        }
    }

    ErrorString errorDescription;
    QString enex = convertNotesToEnex(errorDescription);
    if (enex.isEmpty()) {
        emit failedToExportNotesToEnex(errorDescription);
        return;
    }

    emit notesExportedToEnex(enex);
}

void NoteEnexExporter::onFindNoteFailed(Note note, bool withResourceBinaryData,
                                        ErrorString errorDescription, QUuid requestId)
{
    auto it = m_findNoteRequestIds.find(requestId);
    if (it == m_findNoteRequestIds.end()) {
        return;
    }

    QNDEBUG(QStringLiteral("NoteEnexExporter::onFindNoteFailed: request id = ")
            << requestId << QStringLiteral(", error: ") << errorDescription
            << QStringLiteral(", note: ") << note);

    Q_UNUSED(withResourceBinaryData)

    ErrorString error(QT_TRANSLATE_NOOP("", "Can't export note(s) to ENEX: "
                                            "can't find one of notes in the local storage"));
    error.additionalBases().append(errorDescription.base());
    error.additionalBases().append(errorDescription.additionalBases());
    error.details() = errorDescription.details();
    QNWARNING(error);

    clear();
    emit failedToExportNotesToEnex(error);
}

void NoteEnexExporter::onAllTagsListed()
{
    QNDEBUG(QStringLiteral("NoteEnexExporter::onAllTagsListed"));

    QObject::disconnect(m_pTagModel.data(), QNSIGNAL(TagModel,allTagsListed),
                        this, QNSLOT(NoteEnexExporter,onAllTagsListed));

    if (m_noteLocalUids.isEmpty()) {
        QNDEBUG(QStringLiteral("No note local uids are specified, won't do anything"));
        return;
    }

    if (!m_findNoteRequestIds.isEmpty()) {
        QNDEBUG(QStringLiteral("Still pending  ") << m_findNoteRequestIds.size()
                << QStringLiteral(" find note in local storage requests "));
        return;
    }

    ErrorString errorDescription;
    QString enex = convertNotesToEnex(errorDescription);
    if (enex.isEmpty()) {
        emit failedToExportNotesToEnex(errorDescription);
        return;
    }

    emit notesExportedToEnex(enex);
}

void NoteEnexExporter::findNoteInLocalStorage(const QString & noteLocalUid)
{
    QNDEBUG(QStringLiteral("NoteEnexExporter::findNoteInLocalStorage: ") << noteLocalUid);

    Note dummyNote;
    dummyNote.setLocalUid(noteLocalUid);

    QUuid requestId = QUuid::createUuid();
    Q_UNUSED(m_findNoteRequestIds.insert(requestId))

    connectToLocalStorage();

    QNTRACE(QStringLiteral("Emitting the request to find note in the local storage: note local uid = ")
            << noteLocalUid << QStringLiteral(", request id = ") << requestId);
    emit findNote(dummyNote, /* with resource binary data */ true, requestId);
}

QString NoteEnexExporter::convertNotesToEnex(ErrorString & errorDescription)
{
    QNDEBUG(QStringLiteral("NoteEnexExporter::convertNotesToEnex"));

    if (m_notesByLocalUid.isEmpty()) {
        errorDescription.base() = QT_TRANSLATE_NOOP("", "Can't export notes to ENEX: no notes were specified or found");
        QNWARNING(errorDescription);
        return QString();
    }

    if (m_includeTags && m_pTagModel.isNull()) {
        errorDescription.base() = QT_TRANSLATE_NOOP("", "Can't export notes to ENEX: the tag model has expired");
        QNWARNING(errorDescription);
        return QString();
    }

    QVector<Note> notes;
    notes.reserve(m_notesByLocalUid.size());

    QHash<QString, QString> tagNameByTagLocalUid;
    for(auto it = m_notesByLocalUid.constBegin(), end = m_notesByLocalUid.constEnd(); it != end; ++it)
    {
        const Note & currentNote = it.value();

        if (m_includeTags && currentNote.hasTagLocalUids())
        {
            const QStringList & tagLocalUids = currentNote.tagLocalUids();
            for(auto tagIt = tagLocalUids.constBegin(), tagEnd = tagLocalUids.constEnd(); tagIt != tagEnd; ++tagIt)
            {
                const TagModelItem * pTagItem = m_pTagModel->itemForLocalUid(*tagIt);
                if (Q_UNLIKELY(!pTagItem)) {
                    errorDescription.base() = QT_TRANSLATE_NOOP("", "Can't export notes to ENEX: internal error, "
                                                                "detected note with tag local uid for which no tag "
                                                                "model item was found");
                    QNWARNING(errorDescription << QStringLiteral(", tag local uid = ") << *tagIt
                              << QStringLiteral(", note: ") << currentNote);
                    return QString();
                }

                tagNameByTagLocalUid[*tagIt] = pTagItem->name();
            }
        }

        notes << currentNote;
    }

    QString enex;
    ENMLConverter converter;
    ENMLConverter::EnexExportTags::type exportTagsOption = (m_includeTags
                                                            ? ENMLConverter::EnexExportTags::Yes
                                                            : ENMLConverter::EnexExportTags::No);
    bool res = converter.exportNotesToEnex(notes, tagNameByTagLocalUid, exportTagsOption,
                                           enex, errorDescription, QUENTIER_ENEX_VERSION);
    if (!res) {
        return QString();
    }

    QNDEBUG(QStringLiteral("Successfully exported note(s) to ENEX"));
    return enex;
}

void NoteEnexExporter::connectToLocalStorage()
{
    QNDEBUG(QStringLiteral("NoteEnexExporter::connectToLocalStorage"));

    if (m_connectedToLocalStorage) {
        QNTRACE(QStringLiteral("Already connected to local storage"));
        return;
    }

    QObject::connect(this, QNSIGNAL(NoteEnexExporter,findNote,Note,bool,QUuid),
                     &m_localStorageWorker, QNSLOT(LocalStorageManagerThreadWorker,onFindNoteRequest,Note,bool,QUuid));
    QObject::connect(&m_localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,findNoteComplete,Note,bool,QUuid),
                     this, QNSLOT(NoteEnexExporter,onFindNoteComplete,Note,bool,QUuid));
    QObject::connect(&m_localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,findNoteFailed,Note,bool,ErrorString,QUuid),
                     this, QNSLOT(NoteEnexExporter,onFindNoteFailed,Note,bool,ErrorString,QUuid));

    m_connectedToLocalStorage = true;
}

void NoteEnexExporter::disconnectFromLocalStorage()
{
    QNDEBUG(QStringLiteral("NoteEnexExporter::disconnectFromLocalStorage"));

    if (!m_connectedToLocalStorage) {
        QNTRACE(QStringLiteral("Not connected to local storage at the moment"));
        return;
    }

    QObject::disconnect(this, QNSIGNAL(NoteEnexExporter,findNote,Note,bool,QUuid),
                        &m_localStorageWorker, QNSLOT(LocalStorageManagerThreadWorker,onFindNoteRequest,Note,bool,QUuid));
    QObject::disconnect(&m_localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,findNoteComplete,Note,bool,QUuid),
                        this, QNSLOT(NoteEnexExporter,onFindNoteComplete,Note,bool,QUuid));
    QObject::disconnect(&m_localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,findNoteFailed,Note,bool,ErrorString,QUuid),
                        this, QNSLOT(NoteEnexExporter,onFindNoteFailed,Note,bool,ErrorString,QUuid));

    m_connectedToLocalStorage = false;
}

} // namespace quentier
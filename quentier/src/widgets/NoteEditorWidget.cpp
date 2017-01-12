/*
 * Copyright 2017 Dmitry Ivanov
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

#include "NoteEditorWidget.h"
#include "../models/TagModel.h"

// Doh, Qt Designer's inability to work with namespaces in the expected way
// is deeply disappointing
#include "NoteTagsWidget.h"
#include "FindAndReplaceWidget.h"
#include "BasicXMLSyntaxHighlighter.h"
#include "../insert-table-tool-button/InsertTableToolButton.h"
#include "../insert-table-tool-button/TableSettingsDialog.h"
#include "../color-picker-tool-button/ColorPickerToolButton.h"
#include <quentier/note_editor/NoteEditor.h>
using quentier::FindAndReplaceWidget;
using quentier::NoteEditor;
using quentier::NoteTagsWidget;
#include "ui_NoteEditorWidget.h"

#include <quentier/local_storage/LocalStorageManagerThreadWorker.h>
#include <quentier/logging/QuentierLogger.h>
#include <quentier/types/Resource.h>
#include <quentier/utility/EventLoopWithExitStatus.h>
#include <quentier/utility/ApplicationSettings.h>
#include <QFontDatabase>
#include <QScopedPointer>
#include <QColor>
#include <QPalette>
#include <QTimer>
#include <QToolTip>

#define CHECK_NOTE_SET() \
    if (Q_UNLIKELY(m_pCurrentNote.isNull()) { \
        emit notifyError(QT_TR_NOOP("No note is set to the editor")); \
        return; \
    }

#define DEFAULT_EDITOR_CONVERT_TO_NOTE_TIMEOUT (5)

namespace quentier {

NoteEditorWidget::NoteEditorWidget(const Account & account, LocalStorageManagerThreadWorker & localStorageWorker,
                                   NoteCache & noteCache, NotebookCache & notebookCache, TagCache & tagCache,
                                   TagModel & tagModel, QUndoStack * pUndoStack, QWidget * parent) :
    QWidget(parent),
    m_pUi(new Ui::NoteEditorWidget),
    m_noteCache(noteCache),
    m_notebookCache(notebookCache),
    m_tagCache(tagCache),
    m_pCurrentNote(),
    m_pCurrentNotebook(),
    m_lastNoteTitleOrPreviewText(),
    m_currentAccount(account),
    m_pUndoStack(pUndoStack),
    m_pConvertToNoteDeadlineTimer(Q_NULLPTR),
    m_findCurrentNoteRequestId(),
    m_findCurrentNotebookRequestId(),
    m_updateNoteRequestIds(),
    m_lastFontSizeComboBoxIndex(-1),
    m_lastFontComboBoxFontFamily(),
    m_lastNoteEditorHtml(),
    m_lastSuggestedFontSize(-1),
    m_lastActualFontSize(-1),
    m_pendingEditorSpellChecker(false),
    m_currentNoteWasExpunged(false),
    m_noteTitleIsEdited(false)
{
    m_pUi->setupUi(this);

    m_pUi->noteEditor->setAccount(m_currentAccount);
    m_pUi->noteEditor->setUndoStack(m_pUndoStack.data());

    setupBlankEditor();

    m_pUi->fontSizeComboBox->clear();
    int numFontSizes = m_pUi->fontSizeComboBox->count();
    QNTRACE(QStringLiteral("fontSizeComboBox num items: ") << numFontSizes);
    for(int i = 0; i < numFontSizes; ++i) {
        QVariant value = m_pUi->fontSizeComboBox->itemData(i, Qt::UserRole);
        QNTRACE(QStringLiteral("Font size value for index[") << i << QStringLiteral("] = ") << value);
    }

    BasicXMLSyntaxHighlighter * highlighter = new BasicXMLSyntaxHighlighter(m_pUi->noteSourceView->document());
    Q_UNUSED(highlighter);

    m_pUi->tagNameLabelsContainer->setTagModel(&tagModel);
    m_pUi->tagNameLabelsContainer->setLocalStorageManagerThreadWorker(localStorageWorker);
    createConnections(localStorageWorker);

    QWidget::setAttribute(Qt::WA_DeleteOnClose, /* on = */ true);
}

NoteEditorWidget::~NoteEditorWidget()
{
    delete m_pUi;
}

QString NoteEditorWidget::noteLocalUid() const
{
    if (m_pCurrentNote.isNull()) {
        return QString();
    }

    return m_pCurrentNote->localUid();
}

void NoteEditorWidget::setNoteLocalUid(const QString & noteLocalUid)
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::setNoteLocalUid: ") << noteLocalUid);

    if (!m_pCurrentNote.isNull() && (m_pCurrentNote->localUid() == noteLocalUid)) {
        QNDEBUG(QStringLiteral("This note is already set to the editor, nothing to do"));
        return;
    }

    clear();

    if (noteLocalUid.isEmpty()) {
        setupBlankEditor();
        return;
    }

    const Note * pCachedNote = m_noteCache.get(noteLocalUid);

    // The cache might contain the note without resource binary data, need to check for this
    bool hasMissingResources = false;
    if (Q_LIKELY(pCachedNote))
    {
        QList<Resource> resources = pCachedNote->resources();
        if (!resources.isEmpty())
        {
            for(int i = 0, size = resources.size(); i < size; ++i)
            {
                const Resource & resource = resources[i];
                if (resource.hasDataHash() && !resource.hasDataBody()) {
                    hasMissingResources = true;
                    break;
                }
            }
        }
    }

    if (Q_UNLIKELY(!pCachedNote || hasMissingResources))
    {
        m_findCurrentNoteRequestId = QUuid::createUuid();
        Note dummy;
        dummy.setLocalUid(noteLocalUid);
        QNTRACE(QStringLiteral("Emitting the request to find the current note: local uid = ") << noteLocalUid
                << QStringLiteral(", request id = ") << m_findCurrentNoteRequestId);
        emit findNote(dummy, /* with resource binary data = */ true, m_findCurrentNoteRequestId);
        return;
    }

    QNTRACE(QStringLiteral("Found the cached note"));
    if (Q_UNLIKELY(!pCachedNote->hasNotebookLocalUid() && !pCachedNote->hasNotebookGuid())) {
        emit notifyError(QT_TR_NOOP("Can't set the note to the editor: the note has no linkage to any notebook"));
        return;
    }

    m_pCurrentNote.reset(new Note(*pCachedNote));

    const Notebook * pCachedNotebook = Q_NULLPTR;
    if (m_pCurrentNote->hasNotebookLocalUid()) {
        pCachedNotebook = m_notebookCache.get(m_pCurrentNote->notebookLocalUid());
    }

    if (Q_UNLIKELY(!pCachedNotebook))
    {
        m_findCurrentNotebookRequestId = QUuid::createUuid();
        Notebook dummy;
        if (m_pCurrentNote->hasNotebookLocalUid()) {
            dummy.setLocalUid(m_pCurrentNote->notebookLocalUid());
        }
        else {
            dummy.setLocalUid(QString());
            dummy.setGuid(m_pCurrentNote->notebookGuid());
        }

        QNTRACE(QStringLiteral("Emitting the request to find the current notebook: ") << dummy
                << QStringLiteral("\nRequest id = ") << m_findCurrentNotebookRequestId);
        emit findNotebook(dummy, m_findCurrentNotebookRequestId);
        return;
    }

    m_pCurrentNotebook.reset(new Notebook(*pCachedNotebook));

    setNoteAndNotebook(*m_pCurrentNote, *m_pCurrentNotebook);
    emit resolved();
}

bool NoteEditorWidget::isResolved() const
{
    return !m_pCurrentNote.isNull() && !m_pCurrentNotebook.isNull();
}

QString NoteEditorWidget::titleOrPreview() const
{
    if (Q_UNLIKELY(m_pCurrentNote.isNull())) {
        return QString();
    }

    if (m_pCurrentNote->hasTitle()) {
        return m_pCurrentNote->title();
    }

    if (m_pCurrentNote->hasContent()) {
        QString previewText = m_pCurrentNote->plainText();
        previewText.truncate(140);
        return previewText;
    }

    return QString();
}

bool NoteEditorWidget::isNoteSourceShown() const
{
    return m_pUi->noteSourceView->isVisible();
}

void NoteEditorWidget::showNoteSource()
{
    updateNoteSourceView(m_lastNoteEditorHtml);
    m_pUi->noteSourceView->setHidden(false);
}

void NoteEditorWidget::hideNoteSource()
{
    m_pUi->noteSourceView->setHidden(true);
}

bool NoteEditorWidget::isSpellCheckEnabled() const
{
    return m_pUi->noteEditor->spellCheckEnabled();
}

NoteEditorWidget::NoteSaveStatus::type NoteEditorWidget::checkAndSaveModifiedNote(QNLocalizedString & errorDescription)
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::checkAndSaveModifiedNote"));

    if (m_pCurrentNote.isNull()) {
        QNDEBUG(QStringLiteral("No note is set to the editor"));
        return NoteSaveStatus::Ok;
    }

    if (!m_pUi->noteEditor->isModified()) {
        QNDEBUG(QStringLiteral("Note is not modified, nothing to save"));
        return NoteSaveStatus::Ok;
    }

    ApplicationSettings appSettings;
    appSettings.beginGroup(QStringLiteral("NoteEditor"));
    QVariant editorConvertToNoteTimeoutData = appSettings.value(QStringLiteral("ConvertToNoteTimeout"));
    appSettings.endGroup();

    bool conversionResult = false;
    int editorConvertToNoteTimeout = editorConvertToNoteTimeoutData.toInt(&conversionResult);
    if (!conversionResult) {
        QNDEBUG(QStringLiteral("Can't read the timeout for note editor to note conversion from the application settings, "
                               "fallback to the default value of ") << DEFAULT_EDITOR_CONVERT_TO_NOTE_TIMEOUT
                << QStringLiteral(" seconds"));
        editorConvertToNoteTimeout = DEFAULT_EDITOR_CONVERT_TO_NOTE_TIMEOUT;
    }
    else {
        editorConvertToNoteTimeout = std::max(editorConvertToNoteTimeout, 1);
    }

    if (m_pConvertToNoteDeadlineTimer) {
        m_pConvertToNoteDeadlineTimer->deleteLater();
    }

    m_pConvertToNoteDeadlineTimer = new QTimer(this);
    m_pConvertToNoteDeadlineTimer->setSingleShot(true);

    EventLoopWithExitStatus eventLoop;
    QObject::connect(m_pConvertToNoteDeadlineTimer, QNSIGNAL(QTimer,timeout),
                     &eventLoop, QNSLOT(EventLoopWithExitStatus,exitAsTimeout));
    QObject::connect(this, QNSIGNAL(NoteEditorWidget,noteSavedInLocalStorage),
                     &eventLoop, QNSLOT(EventLoopWithExitStatus,exitAsSuccess));
    QObject::connect(this, QNSIGNAL(NoteEditorWidget,noteSaveInLocalStorageFailed),
                     &eventLoop, QNSLOT(EventLoopWithExitStatus,exitAsFailure));
    QObject::connect(this, QNSIGNAL(NoteEditorWidget,conversionToNoteFailed),
                     &eventLoop, QNSLOT(EventLoopWithExitStatus,exitAsFailure));

    m_pConvertToNoteDeadlineTimer->start(editorConvertToNoteTimeout);

    QTimer::singleShot(0, m_pUi->noteEditor, SLOT(convertToNote()));
    int result = eventLoop.exec(QEventLoop::ExcludeUserInputEvents);

    if (result == EventLoopWithExitStatus::ExitStatus::Failure) {
        errorDescription = QT_TR_NOOP("Failed to convert the editor contents to note");
        QNWARNING(errorDescription);
        return NoteSaveStatus::Failed;
    }
    else if (result == EventLoopWithExitStatus::ExitStatus::Timeout) {
        errorDescription = QT_TR_NOOP("The conversion of note editor contents "
                                      "to note failed to finish in time");
        QNWARNING(errorDescription);
        return NoteSaveStatus::Timeout;
    }

    return NoteSaveStatus::Ok;
}

void NoteEditorWidget::setFocusToEditor()
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::setFocusToEditor"));
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::setFocusToTitle()
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::setFocusToTitle"));
    m_pUi->noteNameLineEdit->setFocus();
}

bool NoteEditorWidget::isNoteTitleEdited() const
{
    return m_noteTitleIsEdited;
}

void NoteEditorWidget::closeEvent(QCloseEvent * pEvent)
{
    if (Q_UNLIKELY(!pEvent)) {
        QNWARNING(QStringLiteral("Detected null pointer to QCloseEvent in NoteEditorWidget's closeEvent"));
        return;
    }

    QNLocalizedString errorDescription;
    NoteSaveStatus::type status = checkAndSaveModifiedNote(errorDescription);
    QNDEBUG(QStringLiteral("Check and save modified note, status: ") << status
            << QStringLiteral(", error description: ") << errorDescription);

    pEvent->accept();
}

void NoteEditorWidget::onEditorTextBoldToggled()
{
    m_pUi->noteEditor->textBold();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextItalicToggled()
{
    m_pUi->noteEditor->textItalic();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextUnderlineToggled()
{
    m_pUi->noteEditor->textUnderline();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextStrikethroughToggled()
{
    m_pUi->noteEditor->textStrikethrough();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextAlignLeftAction()
{
    if (m_pUi->formatJustifyLeftPushButton->isChecked()) {
        m_pUi->formatJustifyCenterPushButton->setChecked(false);
        m_pUi->formatJustifyRightPushButton->setChecked(false);
    }

    m_pUi->noteEditor->alignLeft();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextAlignCenterAction()
{
    if (m_pUi->formatJustifyCenterPushButton->isChecked()) {
        m_pUi->formatJustifyLeftPushButton->setChecked(false);
        m_pUi->formatJustifyRightPushButton->setChecked(false);
    }

    m_pUi->noteEditor->alignCenter();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextAlignRightAction()
{
    if (m_pUi->formatJustifyRightPushButton->isChecked()) {
        m_pUi->formatJustifyLeftPushButton->setChecked(false);
        m_pUi->formatJustifyCenterPushButton->setChecked(false);
    }

    m_pUi->noteEditor->alignRight();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextAddHorizontalLineAction()
{
    m_pUi->noteEditor->insertHorizontalLine();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextIncreaseFontSizeAction()
{
    m_pUi->noteEditor->increaseFontSize();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextDecreaseFontSizeAction()
{
    m_pUi->noteEditor->decreaseFontSize();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextHighlightAction()
{
    m_pUi->noteEditor->textHighlight();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextIncreaseIndentationAction()
{
    m_pUi->noteEditor->increaseIndentation();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextDecreaseIndentationAction()
{
    m_pUi->noteEditor->decreaseIndentation();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextInsertUnorderedListAction()
{
    m_pUi->noteEditor->insertBulletedList();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextInsertOrderedListAction()
{
    m_pUi->noteEditor->insertNumberedList();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextEditHyperlinkAction()
{
    m_pUi->noteEditor->editHyperlinkDialog();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextCopyHyperlinkAction()
{
    m_pUi->noteEditor->copyHyperlink();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorTextRemoveHyperlinkAction()
{
    m_pUi->noteEditor->removeHyperlink();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorChooseTextColor(QColor color)
{
    m_pUi->noteEditor->setFontColor(color);
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorChooseBackgroundColor(QColor color)
{
    m_pUi->noteEditor->setBackgroundColor(color);
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorSpellCheckStateChanged(int state)
{
    m_pUi->noteEditor->setSpellcheck(state != Qt::Unchecked);
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorInsertToDoCheckBoxAction()
{
    m_pUi->noteEditor->insertToDoCheckbox();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorInsertTableDialogAction()
{
    onEditorTextInsertTableDialogRequested();
}

void NoteEditorWidget::onEditorInsertTable(int rows, int columns, double width, bool relativeWidth)
{
    rows = std::max(rows, 1);
    columns = std::max(columns, 1);
    width = std::max(width, 1.0);

    if (relativeWidth) {
        m_pUi->noteEditor->insertRelativeWidthTable(rows, columns, width);
    }
    else {
        m_pUi->noteEditor->insertFixedWidthTable(rows, columns, static_cast<int>(width));
    }

    QNTRACE(QStringLiteral("Inserted table: rows = ") << rows << QStringLiteral(", columns = ") << columns
            << QStringLiteral(", width = ") << width << QStringLiteral(", relative width = ")
            << (relativeWidth ? QStringLiteral("true") : QStringLiteral("false")));
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onUndoAction()
{
    m_pUi->noteEditor->undo();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onRedoAction()
{
    m_pUi->noteEditor->redo();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onCopyAction()
{
    m_pUi->noteEditor->copy();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onCutAction()
{
    m_pUi->noteEditor->cut();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onPasteAction()
{
    m_pUi->noteEditor->paste();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onSelectAllAction()
{
    m_pUi->noteEditor->selectAll();
    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onUpdateNoteComplete(Note note, bool updateResources, bool updateTags, QUuid requestId)
{
    if (!m_pCurrentNote || (m_pCurrentNote->localUid() != note.localUid())) {
        return;
    }

    QNDEBUG(QStringLiteral("NoteEditorWidget::onUpdateNoteComplete: note local uid = ") << note.localUid()
            << QStringLiteral(", request id = ") << requestId << QStringLiteral(", update resources = ")
            << (updateResources ? QStringLiteral("true") : QStringLiteral("false"))
            << QStringLiteral(", update tags = ") << (updateTags ? QStringLiteral("true") : QStringLiteral("false")));
    QNTRACE(QStringLiteral("Updated note: ") << note);

    auto it = m_updateNoteRequestIds.find(requestId);
    if (it != m_updateNoteRequestIds.end()) {
        Q_UNUSED(m_updateNoteRequestIds.erase(it))
        emit noteSavedInLocalStorage();
    }

    QList<Resource> backupResources;
    if (!updateResources) {
        backupResources = m_pCurrentNote->resources();
    }

    QStringList backupTagLocalUids;
    bool hadTagLocalUids = m_pCurrentNote->hasTagLocalUids();
    if (!updateTags && hadTagLocalUids) {
        backupTagLocalUids = m_pCurrentNote->tagLocalUids();
    }

    QStringList backupTagGuids;
    bool hadTagGuids = m_pCurrentNote->hasTagGuids();
    if (!updateTags && hadTagGuids) {
        backupTagGuids = m_pCurrentNote->tagGuids();
    }

    *m_pCurrentNote = note;

    if (!updateResources) {
        m_pCurrentNote->setResources(backupResources);
    }

    if (!updateTags) {
        m_pCurrentNote->setTagLocalUids(backupTagLocalUids);
        m_pCurrentNote->setTagGuids(backupTagGuids);
    }

    if (Q_UNLIKELY(m_pCurrentNotebook.isNull()))
    {
        QNDEBUG(QStringLiteral("Current notebook is null - a bit unexpected at this point"));

        if (!m_findCurrentNotebookRequestId.isNull()) {
            QNDEBUG(QStringLiteral("The request to find the current notebook is still active, waiting for it to finish"));
            return;
        }

        const Notebook * pCachedNotebook = Q_NULLPTR;
        if (m_pCurrentNote->hasNotebookLocalUid()) {
            const QString & notebookLocalUid = m_pCurrentNote->notebookLocalUid();
            pCachedNotebook = m_notebookCache.get(notebookLocalUid);
        }

        if (pCachedNotebook)
        {
            m_pCurrentNotebook.reset(new Notebook(*pCachedNotebook));
        }
        else
        {
            if (Q_UNLIKELY(!m_pCurrentNote->hasNotebookLocalUid() && !m_pCurrentNote->hasNotebookGuid())) {
                QNLocalizedString error = QT_TR_NOOP("Note");
                error += QStringLiteral(" ");
                if (note.hasTitle()) {
                    error += QStringLiteral("\"");
                    error += note.title();
                    error += QStringLiteral("\"");
                }
                else {
                    error += QT_TR_NOOP("with local uid");
                    error += QStringLiteral(" ");
                    error += m_pCurrentNote->localUid();
                }

                error += QStringLiteral(" ");
                error += QT_TR_NOOP("has neither notebook local uid nor notebook guid");
                QNWARNING(error << QStringLiteral(", note: ") << *m_pCurrentNote);
                emit notifyError(error);
                clear();
                return;
            }

            m_findCurrentNotebookRequestId = QUuid::createUuid();
            Notebook dummy;
            if (m_pCurrentNote->hasNotebookLocalUid()) {
                dummy.setLocalUid(m_pCurrentNote->notebookLocalUid());
            }
            else {
                dummy.setLocalUid(QString());
                dummy.setGuid(m_pCurrentNote->notebookGuid());
            }

            QNTRACE(QStringLiteral("Emitting the request to find the current notebook: ") << dummy
                    << QStringLiteral("\nRequest id = ") << m_findCurrentNotebookRequestId);
            emit findNotebook(dummy, m_findCurrentNotebookRequestId);
            return;
        }
    }

    setNoteAndNotebook(*m_pCurrentNote, *m_pCurrentNotebook);
}

void NoteEditorWidget::onUpdateNoteFailed(Note note, bool updateResources, bool updateTags,
                                          QNLocalizedString errorDescription, QUuid requestId)
{
    auto it = m_updateNoteRequestIds.find(requestId);
    if (it == m_updateNoteRequestIds.end()) {
        return;
    }

    QNWARNING(QStringLiteral("NoteEditorWidget::onUpdateNoteFailed: ") << note << QStringLiteral(", update resoures = ")
              << (updateResources ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", update tags = ")
              << (updateTags ? QStringLiteral("true") : QStringLiteral("false"))
              << QStringLiteral(", error description: ") << errorDescription << QStringLiteral("\nRequest id = ") << requestId);

    QNLocalizedString error = QT_TR_NOOP("Failed to save the updated note");
    error += QStringLiteral(": ");
    error += errorDescription;
    emit notifyError(error);
    // NOTE: not clearing out the unsaved stuff because it may be of value to the user

    emit noteSaveInLocalStorageFailed();
}

void NoteEditorWidget::onFindNoteComplete(Note note, bool withResourceBinaryData, QUuid requestId)
{
    if (requestId != m_findCurrentNoteRequestId) {
        return;
    }

    QNDEBUG(QStringLiteral("NoteEditorWidget::onFindNoteComplete: request id = ") << requestId
            << QStringLiteral(", with resource binary data = ") << (withResourceBinaryData ? QStringLiteral("true") : QStringLiteral("false")));
    QNTRACE(QStringLiteral("Note: ") << note);

    m_findCurrentNoteRequestId = QUuid();

    m_pCurrentNote.reset(new Note(note));

    const Notebook * pCachedNotebook = Q_NULLPTR;
    if (m_pCurrentNote->hasNotebookLocalUid()) {
        pCachedNotebook = m_notebookCache.get(m_pCurrentNote->notebookLocalUid());
    }

    if (Q_UNLIKELY(!pCachedNotebook))
    {
        if (!m_findCurrentNotebookRequestId.isNull()) {
            QNDEBUG(QStringLiteral("Couldn't find the notebook in the cache and the request to find it is already active"));
            return;
        }

        m_findCurrentNotebookRequestId = QUuid::createUuid();
        Notebook dummy;
        if (m_pCurrentNote->hasNotebookLocalUid()) {
            dummy.setLocalUid(m_pCurrentNote->notebookLocalUid());
        }
        else {
            dummy.setLocalUid(QString());
            dummy.setGuid(m_pCurrentNote->notebookGuid());
        }

        QNTRACE(QStringLiteral("Emitting the request to find the current notebook: ") << dummy
                << QStringLiteral("\nRequest id = ") << m_findCurrentNotebookRequestId);
        emit findNotebook(dummy, m_findCurrentNotebookRequestId);
        return;
    }

    m_pCurrentNotebook.reset(new Notebook(*pCachedNotebook));

    setNoteAndNotebook(*m_pCurrentNote, *m_pCurrentNotebook);
    emit resolved();
}

void NoteEditorWidget::onFindNoteFailed(Note note, bool withResourceBinaryData, QNLocalizedString errorDescription,
                                        QUuid requestId)
{
    if (requestId != m_findCurrentNoteRequestId) {
        return;
    }

    QNDEBUG(QStringLiteral("NoteEditorWidget::onFindNoteFailed: request id = ") << requestId << QStringLiteral(", with resource binary data = ")
            << (withResourceBinaryData ? QStringLiteral("true") : QStringLiteral("false")) << QStringLiteral(", error description: ")
            << errorDescription);
    QNTRACE(QStringLiteral("Note: ") << note);

    m_findCurrentNoteRequestId = QUuid();

    clear();

    emit notifyError(QT_TR_NOOP("Can't find the note attempted to be selected in the note editor"));
}

void NoteEditorWidget::onExpungeNoteComplete(Note note, QUuid requestId)
{
    if (!m_pCurrentNote || (m_pCurrentNote->localUid() != note.localUid())) {
        return;
    }

    QNDEBUG(QStringLiteral("NoteEditorWidget::onExpungeNoteComplete: note local uid = ") << note.localUid()
            << QStringLiteral(", request id = ") << requestId);

    m_currentNoteWasExpunged = true;

    // TODO: ideally should allow the choice to either re-save the note or to drop it

    QNLocalizedString message = QT_TR_NOOP("The note loaded into the editor was expunged from the local storage");
    QNINFO(message << QStringLiteral(", note: ") << *m_pCurrentNote);
    emit notifyError(message);
}

void NoteEditorWidget::onUpdateNotebookComplete(Notebook notebook, QUuid requestId)
{
    if (!m_pCurrentNote || !m_pCurrentNotebook || (m_pCurrentNotebook->localUid() != notebook.localUid())) {
        return;
    }

    QNDEBUG(QStringLiteral("NoteEditorWidget::onUpdateNotebookComplete: notebook = ") << notebook << QStringLiteral("\nRequest id = ")
            << requestId);

    setNoteAndNotebook(*m_pCurrentNote, *m_pCurrentNotebook);
}

void NoteEditorWidget::onExpungeNotebookComplete(Notebook notebook, QUuid requestId)
{
    if (!m_pCurrentNotebook || (m_pCurrentNotebook->localUid() != notebook.localUid())) {
        return;
    }

    QNDEBUG(QStringLiteral("NoteEditorWidget::onExpungeNotebookComplete: notebook = ") << notebook
            << QStringLiteral("\nRequest id = ") << requestId);

    clear();
}

void NoteEditorWidget::onFindNotebookComplete(Notebook notebook, QUuid requestId)
{
    if (requestId != m_findCurrentNotebookRequestId) {
        return;
    }

    QNDEBUG(QStringLiteral("NoteEditorWidget::onFindNotebookComplete: request id = ") << requestId
            << QStringLiteral(", notebook: ") << notebook);

    m_findCurrentNotebookRequestId = QUuid();

    if (Q_UNLIKELY(m_pCurrentNote.isNull())) {
        QNDEBUG(QStringLiteral("Can't process the update of the notebook: no current note is set"));
        clear();
        return;
    }

    m_pCurrentNotebook.reset(new Notebook(notebook));

    setNoteAndNotebook(*m_pCurrentNote, *m_pCurrentNotebook);
    emit resolved();
}

void NoteEditorWidget::onFindNotebookFailed(Notebook notebook, QNLocalizedString errorDescription, QUuid requestId)
{
    if (requestId != m_findCurrentNotebookRequestId) {
        return;
    }

    QNDEBUG(QStringLiteral("NoteEditorWidget::onFindNotebookFailed: request id = ") << requestId
            << QStringLiteral(", notebook: ") << notebook << QStringLiteral("\nError description = ") << errorDescription);

    m_findCurrentNotebookRequestId = QUuid();
    clear();
    emit notifyError(QT_TR_NOOP("Can't find the note attempted to be selected in the note editor"));
}

void NoteEditorWidget::onNoteTitleEdited(const QString & noteTitle)
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onNoteTitleEdited: ") << noteTitle);
    m_noteTitleIsEdited = true;
}

void NoteEditorWidget::onNoteTitleUpdated()
{
    QString noteTitle = m_pUi->noteNameLineEdit->text().trimmed();
    QNDEBUG(QStringLiteral("NoteEditorWidget::onNoteTitleUpdated: ") << noteTitle);

    m_noteTitleIsEdited = false;

    if (Q_UNLIKELY(!m_pCurrentNote)) {
        // That shouldn't really happen in normal circumstances but it could in theory be some old event
        // which has reached this object after the note has already been cleaned up
        QNDEBUG(QStringLiteral("No current note in the note editor widget! Ignoring the note title update"));
        return;
    }

    if (m_pCurrentNote->hasTitle() && (m_pCurrentNote->title() == noteTitle)) {
        QNDEBUG(QStringLiteral("Note's title hasn't changed, nothing to do"));
        return;
    }

    if (!noteTitle.isEmpty())
    {
        int noteTitleSize = noteTitle.size();

        if (noteTitleSize < qevercloud::EDAM_NOTE_TITLE_LEN_MIN) {
            QNDEBUG(QStringLiteral("Too short title, should be at least ") << qevercloud::EDAM_NOTE_TITLE_LEN_MIN);
            QToolTip::showText(m_pUi->noteNameLineEdit->mapToGlobal(QPoint(0, m_pUi->noteNameLineEdit->height())),
                               tr("Too short title, should be at least") + QStringLiteral(" ") + QString::number(qevercloud::EDAM_NOTE_TITLE_LEN_MIN));
            return;
        }

        if (noteTitleSize > qevercloud::EDAM_NOTE_TITLE_LEN_MAX) {
            QNDEBUG(QStringLiteral("Too long title, should be no longer than ") << qevercloud::EDAM_NOTE_TITLE_LEN_MAX);
            QToolTip::showText(m_pUi->noteNameLineEdit->mapToGlobal(QPoint(0, m_pUi->noteNameLineEdit->height())),
                               tr("Too long title, max allowed length is") + QStringLiteral(" ") + QString::number(qevercloud::EDAM_NOTE_TITLE_LEN_MAX));
            return;
        }
    }

    m_pCurrentNote->setTitle(noteTitle);

    qevercloud::NoteAttributes & attributes = m_pCurrentNote->noteAttributes();
    attributes.noteTitleQuality.clear();    // indicate learly that the note's title has been changed manually and not generated automatically

    QUuid requestId = QUuid::createUuid();
    Q_UNUSED(m_updateNoteRequestIds.insert(requestId))
    QNTRACE(QStringLiteral("Emitting the request to update note due to note title update: request id = ") << requestId
            << QStringLiteral(", note = ") << *m_pCurrentNote);
    emit updateNote(*m_pCurrentNote, /* update resources = */ true, /* update tags = */ false, requestId);

    emit titleOrPreviewChanged(noteTitle);
}

void NoteEditorWidget::onEditorNoteUpdate(Note note)
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onEditorNoteUpdate: note local uid = ") << note.localUid());
    QNTRACE(QStringLiteral("Note: ") << note);

    if (Q_UNLIKELY(!m_pCurrentNote)) {
        // That shouldn't really happen in normal circumstances but it could in theory be some old event
        // which has reached this object after the note has already been cleaned up
        QNDEBUG(QStringLiteral("No current note in the note editor widget! Ignoring the update from the note editor"));
        return;
    }

    *m_pCurrentNote = note;

    if (Q_LIKELY(m_pCurrentNotebook)) {
        // Update the note within the tag names container just to avoid any potential race with updates from it
        // messing with other note's parts than tags
        m_pUi->tagNameLabelsContainer->setCurrentNoteAndNotebook(*m_pCurrentNote, *m_pCurrentNotebook);
    }

    QUuid requestId = QUuid::createUuid();
    Q_UNUSED(m_updateNoteRequestIds.insert(requestId))
    QNTRACE(QStringLiteral("Emitting the request to update note: request id = ") << requestId
            << QStringLiteral(", note = ") << *m_pCurrentNote);
    emit updateNote(*m_pCurrentNote, /* update resources = */ true, /* update tags = */ false, requestId);
}

void NoteEditorWidget::onEditorNoteUpdateFailed(QNLocalizedString error)
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onEditorNoteUpdateFailed: ") << error);
    emit notifyError(error);

    emit conversionToNoteFailed();
}

void NoteEditorWidget::onEditorTextBoldStateChanged(bool state)
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onEditorTextBoldStateChanged: ")
            << (state ? QStringLiteral("enabled") : QStringLiteral("disabled")));

    m_pUi->fontBoldPushButton->setChecked(state);
}

void NoteEditorWidget::onEditorTextItalicStateChanged(bool state)
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onEditorTextItalicStateChanged: ")
            << (state ? QStringLiteral("enabled") : QStringLiteral("disabled")));

    m_pUi->fontItalicPushButton->setChecked(state);
}

void NoteEditorWidget::onEditorTextUnderlineStateChanged(bool state)
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onEditorTextUnderlineStateChanged: ")
            << (state ? QStringLiteral("enabled") : QStringLiteral("disabled")));

    m_pUi->fontUnderlinePushButton->setChecked(state);
}

void NoteEditorWidget::onEditorTextStrikethroughStateChanged(bool state)
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onEditorTextStrikethroughStateChanged: ")
            << (state ? QStringLiteral("enabled") : QStringLiteral("disabled")));

    m_pUi->fontStrikethroughPushButton->setChecked(state);
}

void NoteEditorWidget::onEditorTextAlignLeftStateChanged(bool state)
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onEditorTextAlignLeftStateChanged: ")
            << (state ? QStringLiteral("enabled") : QStringLiteral("disabled")));

    m_pUi->formatJustifyLeftPushButton->setChecked(state);

    if (state) {
        m_pUi->formatJustifyCenterPushButton->setChecked(false);
        m_pUi->formatJustifyRightPushButton->setChecked(false);
    }
}

void NoteEditorWidget::onEditorTextAlignCenterStateChanged(bool state)
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onEditorTextAlignCenterStateChanged: ")
            << (state ? QStringLiteral("enabled") : QStringLiteral("disabled")));

    m_pUi->formatJustifyCenterPushButton->setChecked(state);

    if (state) {
        m_pUi->formatJustifyLeftPushButton->setChecked(false);
        m_pUi->formatJustifyRightPushButton->setChecked(false);
    }
}

void NoteEditorWidget::onEditorTextAlignRightStateChanged(bool state)
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onEditorTextAlignRightStateChanged: ")
            << (state ? QStringLiteral("enabled") : QStringLiteral("disabled")));

    m_pUi->formatJustifyRightPushButton->setChecked(state);

    if (state) {
        m_pUi->formatJustifyLeftPushButton->setChecked(false);
        m_pUi->formatJustifyCenterPushButton->setChecked(false);
    }
}

void NoteEditorWidget::onEditorTextInsideOrderedListStateChanged(bool state)
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onEditorTextInsideOrderedListStateChanged: ")
            << (state ? QStringLiteral("enabled") : QStringLiteral("disabled")));

    m_pUi->formatListOrderedPushButton->setChecked(state);

    if (state) {
        m_pUi->formatListUnorderedPushButton->setChecked(false);
    }
}

void NoteEditorWidget::onEditorTextInsideUnorderedListStateChanged(bool state)
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onEditorTextInsideUnorderedListStateChanged: ")
            << (state ? QStringLiteral("enabled") : QStringLiteral("disabled")));

    m_pUi->formatListUnorderedPushButton->setChecked(state);

    if (state) {
        m_pUi->formatListOrderedPushButton->setChecked(false);
    }
}

void NoteEditorWidget::onEditorTextInsideTableStateChanged(bool state)
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onEditorTextInsideTableStateChanged: ")
            << (state ? QStringLiteral("enabled") : QStringLiteral("disabled")));

    m_pUi->insertTableToolButton->setEnabled(!state);
}

void NoteEditorWidget::onEditorTextFontFamilyChanged(QString fontFamily)
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onEditorTextFontFamilyChanged: ") << fontFamily);

    if (m_lastFontComboBoxFontFamily == fontFamily) {
        QNTRACE(QStringLiteral("Font family didn't change"));
        return;
    }

    m_lastFontComboBoxFontFamily = fontFamily;

    QFont currentFont(fontFamily);
    m_pUi->fontComboBox->setCurrentFont(currentFont);
    QNTRACE(QStringLiteral("Font family from combo box: ") << m_pUi->fontComboBox->currentFont().family()
            << QStringLiteral(", font family set by QFont's constructor from it: ") << currentFont.family());

    QFontDatabase fontDatabase;
    QList<int> fontSizes = fontDatabase.pointSizes(currentFont.family(), currentFont.styleName());
    // NOTE: it is important to use currentFont.family() in the call above instead of fontFamily variable
    // because the two can be different by presence/absence of apostrophes around the font family name
    if (fontSizes.isEmpty()) {
        QNTRACE(QStringLiteral("Couldn't find point sizes for font family ") << currentFont.family()
                << QStringLiteral(", will use standard sizes instead"));
        fontSizes = fontDatabase.standardSizes();
    }

    m_lastFontSizeComboBoxIndex = 0;    // NOTE: clearing out font sizes combo box causes unwanted update of its index to 0, workarounding it
    m_pUi->fontSizeComboBox->clear();
    int numFontSizes = fontSizes.size();
    QNTRACE(QStringLiteral("Found ") << numFontSizes << QStringLiteral(" font sizes for font family ") << currentFont.family());

    for(int i = 0; i < numFontSizes; ++i) {
        m_pUi->fontSizeComboBox->addItem(QString::number(fontSizes[i]), QVariant(fontSizes[i]));
        QNTRACE(QStringLiteral("Added item ") << fontSizes[i] << QStringLiteral("pt for index ") << i);
    }

    m_lastFontSizeComboBoxIndex = -1;
}

void NoteEditorWidget::onEditorTextFontSizeChanged(int fontSize)
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onEditorTextFontSizeChanged: ") << fontSize);

    if (m_lastSuggestedFontSize == fontSize) {
        QNTRACE(QStringLiteral("This font size has already been suggested previously"));
        return;
    }

    m_lastSuggestedFontSize = fontSize;

    int fontSizeIndex = m_pUi->fontSizeComboBox->findData(QVariant(fontSize), Qt::UserRole);
    if (fontSizeIndex >= 0)
    {
        m_lastFontSizeComboBoxIndex = fontSizeIndex;
        m_lastActualFontSize = fontSize;

        if (m_pUi->fontSizeComboBox->currentIndex() != fontSizeIndex)
        {
            m_pUi->fontSizeComboBox->setCurrentIndex(fontSizeIndex);
            QNTRACE(QStringLiteral("fontSizeComboBox: set current index to ") << fontSizeIndex
                    << QStringLiteral(", found font size = ") << QVariant(fontSize));
        }

        return;
    }

    QNDEBUG(QStringLiteral("Can't find font size ") << fontSize
            << QStringLiteral(" within those listed in font size combobox, will try to choose the closest one instead"));

    const int numFontSizes = m_pUi->fontSizeComboBox->count();
    int currentSmallestDiscrepancy = 1e5;
    int currentClosestIndex = -1;
    int currentClosestFontSize = -1;
    for(int i = 0; i < numFontSizes; ++i)
    {
        QVariant value = m_pUi->fontSizeComboBox->itemData(i, Qt::UserRole);
        bool conversionResult = false;
        int valueInt = value.toInt(&conversionResult);
        if (!conversionResult) {
            QNWARNING(QStringLiteral("Can't convert value from font size combo box to int: ") << value);
            continue;
        }

        int discrepancy = abs(valueInt - fontSize);
        if (currentSmallestDiscrepancy > discrepancy) {
            currentSmallestDiscrepancy = discrepancy;
            currentClosestIndex = i;
            currentClosestFontSize = valueInt;
            QNTRACE(QStringLiteral("Updated current closest index to ") << i << QStringLiteral(": font size = ") << valueInt);
        }
    }

    if (currentClosestIndex < 0) {
        QNDEBUG(QStringLiteral("Couldn't find closest font size to ") << fontSize);
        return;
    }

    QNTRACE(QStringLiteral("Found closest current font size: ") << currentClosestFontSize << QStringLiteral(", index = ") << currentClosestIndex);

    if ( (m_lastFontSizeComboBoxIndex == currentClosestIndex) &&
         (m_lastActualFontSize == currentClosestFontSize) )
    {
        QNTRACE(QStringLiteral("Neither the font size nor its index within the font combo box have changed"));
        return;
    }

    m_lastFontSizeComboBoxIndex = currentClosestIndex;
    m_lastActualFontSize = currentClosestFontSize;

    if (m_pUi->fontSizeComboBox->currentIndex() != currentClosestIndex) {
        m_pUi->fontSizeComboBox->setCurrentIndex(currentClosestIndex);
    }
}

void NoteEditorWidget::onEditorTextInsertTableDialogRequested()
{
    QNTRACE(QStringLiteral("NoteEditorWidget::onEditorTextInsertTableDialogRequested"));

    QScopedPointer<TableSettingsDialog> tableSettingsDialogHolder(new TableSettingsDialog(this));
    TableSettingsDialog * tableSettingsDialog = tableSettingsDialogHolder.data();
    if (tableSettingsDialog->exec() == QDialog::Rejected) {
        QNTRACE(QStringLiteral("Returned from TableSettingsDialog::exec: rejected"));
        return;
    }

    QNTRACE(QStringLiteral("Returned from TableSettingsDialog::exec: accepted"));
    int numRows = tableSettingsDialog->numRows();
    int numColumns = tableSettingsDialog->numColumns();
    double tableWidth = tableSettingsDialog->tableWidth();
    bool relativeWidth = tableSettingsDialog->relativeWidth();

    if (relativeWidth) {
        m_pUi->noteEditor->insertRelativeWidthTable(numRows, numColumns, tableWidth);
    }
    else {
        m_pUi->noteEditor->insertFixedWidthTable(numRows, numColumns, static_cast<int>(tableWidth));
    }

    m_pUi->noteEditor->setFocus();
}

void NoteEditorWidget::onEditorSpellCheckerNotReady()
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onEditorSpellCheckerNotReady"));

    m_pendingEditorSpellChecker = true;
    emit notifyError(QNLocalizedString("Spell checker is loading dictionaries, please wait", this));
}

void NoteEditorWidget::onEditorSpellCheckerReady()
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onEditorSpellCheckerReady"));

    if (!m_pendingEditorSpellChecker) {
        return;
    }

    m_pendingEditorSpellChecker = false;
    emit notifyError(QNLocalizedString());     // Send the empty message to remove the previous one about the non-ready spell checker
}

void NoteEditorWidget::onEditorHtmlUpdate(QString html)
{
    m_lastNoteEditorHtml = html;

    if (!m_pUi->noteSourceView->isVisible()) {
        return;
    }

    updateNoteSourceView(html);
}

void NoteEditorWidget::onFindInsideNoteAction()
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onFindInsideNoteAction"));

    if (m_pUi->findAndReplaceWidget->isHidden())
    {
        QString textToFind = m_pUi->noteEditor->selectedText();
        if (textToFind.isEmpty()) {
            textToFind = m_pUi->findAndReplaceWidget->textToFind();
        }
        else {
            m_pUi->findAndReplaceWidget->setTextToFind(textToFind);
        }

        m_pUi->findAndReplaceWidget->setHidden(false);
        m_pUi->findAndReplaceWidget->show();
    }

    onFindNextInsideNote(m_pUi->findAndReplaceWidget->textToFind(),
                         m_pUi->findAndReplaceWidget->matchCase());
}

void NoteEditorWidget::onFindPreviousInsideNoteAction()
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onFindPreviousInsideNoteAction"));

    if (m_pUi->findAndReplaceWidget->isHidden())
    {
        QString textToFind = m_pUi->noteEditor->selectedText();
        if (textToFind.isEmpty()) {
            textToFind = m_pUi->findAndReplaceWidget->textToFind();
        }
        else {
            m_pUi->findAndReplaceWidget->setTextToFind(textToFind);
        }

        m_pUi->findAndReplaceWidget->setHidden(false);
        m_pUi->findAndReplaceWidget->show();
    }

    onFindPreviousInsideNote(m_pUi->findAndReplaceWidget->textToFind(),
                             m_pUi->findAndReplaceWidget->matchCase());
}

void NoteEditorWidget::onReplaceInsideNoteAction()
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onReplaceInsideNoteAction"));

    if (m_pUi->findAndReplaceWidget->isHidden() ||
        !m_pUi->findAndReplaceWidget->replaceEnabled())
    {
        QNTRACE(QStringLiteral("At least the replacement part of find and replace widget "
                               "is hidden, will only show it and do nothing else"));

        QString textToFind = m_pUi->noteEditor->selectedText();
        if (textToFind.isEmpty()) {
            textToFind = m_pUi->findAndReplaceWidget->textToFind();
        }
        else {
            m_pUi->findAndReplaceWidget->setTextToFind(textToFind);
        }

        m_pUi->findAndReplaceWidget->setHidden(false);
        m_pUi->findAndReplaceWidget->setReplaceEnabled(true);
        m_pUi->findAndReplaceWidget->show();
        return;
    }

    onReplaceInsideNote(m_pUi->findAndReplaceWidget->textToFind(),
                        m_pUi->findAndReplaceWidget->replacementText(),
                        m_pUi->findAndReplaceWidget->matchCase());
}

void NoteEditorWidget::onFindAndReplaceWidgetClosed()
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onFindAndReplaceWidgetClosed"));
    onFindNextInsideNote(QString(), false);
}

void NoteEditorWidget::onTextToFindInsideNoteEdited(const QString & textToFind)
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onTextToFindInsideNoteEdited: ") << textToFind);

    bool matchCase = m_pUi->findAndReplaceWidget->matchCase();
    onFindNextInsideNote(textToFind, matchCase);
}

#define CHECK_FIND_AND_REPLACE_WIDGET_STATE() \
    if (Q_UNLIKELY(m_pUi->findAndReplaceWidget->isHidden())) { \
        QNTRACE(QStringLiteral("Find and replace widget is not shown, nothing to do")); \
        return; \
    }

void NoteEditorWidget::onFindNextInsideNote(const QString & textToFind, const bool matchCase)
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onFindNextInsideNote: text to find = ")
            << textToFind << QStringLiteral(", match case = ")
            << (matchCase ? QStringLiteral("true") : QStringLiteral("false")));

    CHECK_FIND_AND_REPLACE_WIDGET_STATE()
    m_pUi->noteEditor->findNext(textToFind, matchCase);
}

void NoteEditorWidget::onFindPreviousInsideNote(const QString & textToFind, const bool matchCase)
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onFindPreviousInsideNote: text to find = ")
            << textToFind << QStringLiteral(", match case = ")
            << (matchCase ? QStringLiteral("true") : QStringLiteral("false")));

    CHECK_FIND_AND_REPLACE_WIDGET_STATE()
    m_pUi->noteEditor->findPrevious(textToFind, matchCase);
}

void NoteEditorWidget::onFindInsideNoteCaseSensitivityChanged(const bool matchCase)
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onFindInsideNoteCaseSensitivityChanged: match case = ")
            << (matchCase ? QStringLiteral("true") : QStringLiteral("false")));

    CHECK_FIND_AND_REPLACE_WIDGET_STATE()

    QString textToFind = m_pUi->findAndReplaceWidget->textToFind();
    m_pUi->noteEditor->findNext(textToFind, matchCase);
}

void NoteEditorWidget::onReplaceInsideNote(const QString & textToReplace,
                                           const QString & replacementText,
                                           const bool matchCase)
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onReplaceInsideNote: text to replace = ")
            << textToReplace << QStringLiteral(", replacement text = ") << replacementText
            << QStringLiteral(", match case = ") << (matchCase ? QStringLiteral("true") : QStringLiteral("false")));

    CHECK_FIND_AND_REPLACE_WIDGET_STATE()
    m_pUi->findAndReplaceWidget->setReplaceEnabled(true);

    m_pUi->noteEditor->replace(textToReplace, replacementText, matchCase);
}

void NoteEditorWidget::onReplaceAllInsideNote(const QString & textToReplace,
                                              const QString & replacementText,
                                              const bool matchCase)
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::onReplaceAllInsideNote: text to replace = ")
            << textToReplace << QStringLiteral(", replacement text = ") << replacementText
            << QStringLiteral(", match case = ") << (matchCase ? QStringLiteral("true") : QStringLiteral("false")));

    CHECK_FIND_AND_REPLACE_WIDGET_STATE()
    m_pUi->findAndReplaceWidget->setReplaceEnabled(true);

    m_pUi->noteEditor->replaceAll(textToReplace, replacementText, matchCase);
}

#undef CHECK_FIND_AND_REPLACE_WIDGET_STATE

void NoteEditorWidget::createConnections(LocalStorageManagerThreadWorker & localStorageWorker)
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::createConnections"));

    // Local signals to localStorageWorker's slots
    QObject::connect(this, QNSIGNAL(NoteEditorWidget,updateNote,Note,bool,bool,QUuid),
                     &localStorageWorker, QNSLOT(LocalStorageManagerThreadWorker,onUpdateNoteRequest,Note,bool,bool,QUuid));
    QObject::connect(this, QNSIGNAL(NoteEditorWidget,findNote,Note,bool,QUuid),
                     &localStorageWorker, QNSLOT(LocalStorageManagerThreadWorker,onFindNoteRequest,Note,bool,QUuid));
    QObject::connect(this, QNSIGNAL(NoteEditorWidget,findNotebook,Notebook,QUuid),
                     &localStorageWorker, QNSLOT(LocalStorageManagerThreadWorker,onFindNotebookRequest,Notebook,QUuid));

    // localStorageWorker's signals to local slots
    QObject::connect(&localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,updateNoteComplete,Note,bool,bool,QUuid),
                     this, QNSLOT(NoteEditorWidget,onUpdateNoteComplete,Note,bool,bool,QUuid));
    QObject::connect(&localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,updateNoteFailed,Note,bool,bool,QNLocalizedString,QUuid),
                     this, QNSLOT(NoteEditorWidget,onUpdateNoteFailed,Note,bool,bool,QNLocalizedString,QUuid));
    QObject::connect(&localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,findNoteComplete,Note,bool,QUuid),
                     this, QNSLOT(NoteEditorWidget,onFindNoteComplete,Note,bool,QUuid));
    QObject::connect(&localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,findNoteFailed,Note,bool,QNLocalizedString,QUuid),
                     this, QNSLOT(NoteEditorWidget,onFindNoteFailed,Note,bool,QNLocalizedString,QUuid));
    QObject::connect(&localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,expungeNoteComplete,Note,QUuid),
                     this, QNSLOT(NoteEditorWidget,onExpungeNoteComplete,Note,QUuid));
    QObject::connect(&localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,updateNotebookComplete,Notebook,QUuid),
                     this, QNSLOT(NoteEditorWidget,onUpdateNotebookComplete,Notebook,QUuid));
    QObject::connect(&localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,findNotebookComplete,Notebook,QUuid),
                     this, QNSLOT(NoteEditorWidget,onFindNotebookComplete,Notebook,QUuid));
    QObject::connect(&localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,findNotebookFailed,Notebook,QNLocalizedString,QUuid),
                     this, QNSLOT(NoteEditorWidget,onFindNotebookFailed,Notebook,QNLocalizedString,QUuid));
    QObject::connect(&localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,expungeNotebookComplete,Notebook,QUuid),
                     this, QNSLOT(NoteEditorWidget,onExpungeNotebookComplete,Notebook,QUuid));

    // Connect to note title updates
    QObject::connect(m_pUi->noteNameLineEdit, QNSIGNAL(QLineEdit,textEdited,const QString&),
                     this, QNSLOT(NoteEditorWidget,onNoteTitleEdited,const QString&));
    QObject::connect(m_pUi->noteNameLineEdit, QNSIGNAL(QLineEdit,editingFinished),
                     this, QNSLOT(NoteEditorWidget,onNoteTitleUpdated));

    // Connect note editor's signals to local slots
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,convertedToNote,Note),
                     this, QNSLOT(NoteEditorWidget,onEditorNoteUpdate,Note));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,cantConvertToNote,QNLocalizedString),
                     this, QNSLOT(NoteEditorWidget,onEditorNoteUpdateFailed,QNLocalizedString));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,currentNoteChanged,Note),
                     this, QNSLOT(NoteEditorWidget,onEditorNoteUpdate,Note));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,noteEditorHtmlUpdated,QString),
                     this, QNSLOT(NoteEditorWidget,onEditorHtmlUpdate,QString));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,noteLoaded),
                     this, QNSIGNAL(NoteEditorWidget,noteLoaded));

    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,textBoldState,bool),
                     this, QNSLOT(NoteEditorWidget,onEditorTextBoldStateChanged,bool));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,textItalicState,bool),
                     this, QNSLOT(NoteEditorWidget,onEditorTextItalicStateChanged,bool));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,textUnderlineState,bool),
                     this, QNSLOT(NoteEditorWidget,onEditorTextUnderlineStateChanged,bool));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,textUnderlineState,bool),
                     this, QNSLOT(NoteEditorWidget,onEditorTextUnderlineStateChanged,bool));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,textStrikethroughState,bool),
                     this, QNSLOT(NoteEditorWidget,onEditorTextStrikethroughStateChanged,bool));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,textAlignLeftState,bool),
                     this, QNSLOT(NoteEditorWidget,onEditorTextAlignLeftStateChanged,bool));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,textAlignCenterState,bool),
                     this, QNSLOT(NoteEditorWidget,onEditorTextAlignCenterStateChanged,bool));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,textAlignRightState,bool),
                     this, QNSLOT(NoteEditorWidget,onEditorTextAlignRightStateChanged,bool));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,textInsideOrderedListState,bool),
                     this, QNSLOT(NoteEditorWidget,onEditorTextInsideOrderedListStateChanged,bool));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,textInsideUnorderedListState,bool),
                     this, QNSLOT(NoteEditorWidget,onEditorTextInsideUnorderedListStateChanged,bool));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,textInsideTableState,bool),
                     this, QNSLOT(NoteEditorWidget,onEditorTextInsideTableStateChanged,bool));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,textFontFamilyChanged,QString),
                     this, QNSLOT(NoteEditorWidget,onEditorTextFontFamilyChanged,QString));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,textFontSizeChanged,int),
                     this, QNSLOT(NoteEditorWidget,onEditorTextFontSizeChanged,int));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,insertTableDialogRequested),
                     this, QNSLOT(NoteEditorWidget,onEditorTextInsertTableDialogRequested));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,spellCheckerNotReady),
                     this, QNSLOT(NoteEditorWidget,onEditorSpellCheckerNotReady));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,spellCheckerReady),
                     this, QNSLOT(NoteEditorWidget,onEditorSpellCheckerReady));
    QObject::connect(m_pUi->noteEditor, QNSIGNAL(NoteEditor,notifyError,QNLocalizedString),
                     this, QNSIGNAL(NoteEditorWidget,notifyError,QNLocalizedString));

    // Connect find and replace widget actions to local slots
    QObject::connect(m_pUi->findAndReplaceWidget, QNSIGNAL(FindAndReplaceWidget,closed),
                     this, QNSLOT(NoteEditorWidget,onFindAndReplaceWidgetClosed));
    QObject::connect(m_pUi->findAndReplaceWidget, QNSIGNAL(FindAndReplaceWidget,textToFindEdited,const QString&),
                     this, QNSLOT(NoteEditorWidget,onTextToFindInsideNoteEdited,const QString&));
    QObject::connect(m_pUi->findAndReplaceWidget, QNSIGNAL(FindAndReplaceWidget,findNext,const QString&,const bool),
                     this, QNSLOT(NoteEditorWidget,onFindNextInsideNote,const QString&,const bool));
    QObject::connect(m_pUi->findAndReplaceWidget, QNSIGNAL(FindAndReplaceWidget,findPrevious,const QString&,const bool),
                     this, QNSLOT(NoteEditorWidget,onFindPreviousInsideNote,const QString&,const bool));
    QObject::connect(m_pUi->findAndReplaceWidget, QNSIGNAL(FindAndReplaceWidget,searchCaseSensitivityChanged,const bool),
                     this, QNSLOT(NoteEditorWidget,onFindInsideNoteCaseSensitivityChanged,const bool));
    QObject::connect(m_pUi->findAndReplaceWidget, QNSIGNAL(FindAndReplaceWidget,replace,const QString&,const QString&,const bool),
                     this, QNSLOT(NoteEditorWidget,onReplaceInsideNote,const QString&,const QString&,const bool));
    QObject::connect(m_pUi->findAndReplaceWidget, QNSIGNAL(FindAndReplaceWidget,replaceAll,const QString&,const QString&,const bool),
                     this, QNSLOT(NoteEditorWidget,onReplaceAllInsideNote,const QString&,const QString&,const bool));

    // Connect toolbar buttons actions to local slots
    QObject::connect(m_pUi->fontBoldPushButton, QNSIGNAL(QPushButton,released),
                     this, QNSLOT(NoteEditorWidget,onEditorTextBoldToggled));
    QObject::connect(m_pUi->fontItalicPushButton, QNSIGNAL(QPushButton,released),
                     this, QNSLOT(NoteEditorWidget,onEditorTextItalicToggled));
    QObject::connect(m_pUi->fontUnderlinePushButton, QNSIGNAL(QPushButton,released),
                     this, QNSLOT(NoteEditorWidget,onEditorTextUnderlineToggled));
    QObject::connect(m_pUi->fontStrikethroughPushButton, QNSIGNAL(QPushButton,released),
                     this, QNSLOT(NoteEditorWidget,onEditorTextStrikethroughToggled));
    QObject::connect(m_pUi->formatJustifyLeftPushButton, QNSIGNAL(QPushButton,released),
                     this, QNSLOT(NoteEditorWidget,onEditorTextAlignLeftAction));
    QObject::connect(m_pUi->formatJustifyCenterPushButton, QNSIGNAL(QPushButton,released),
                     this, QNSLOT(NoteEditorWidget,onEditorTextAlignCenterAction));
    QObject::connect(m_pUi->formatJustifyRightPushButton, QNSIGNAL(QPushButton,released),
                     this, QNSLOT(NoteEditorWidget,onEditorTextAlignRightAction));
    QObject::connect(m_pUi->insertHorizontalLinePushButton, QNSIGNAL(QPushButton,released),
                     this, QNSLOT(NoteEditorWidget,onEditorTextAddHorizontalLineAction));
    QObject::connect(m_pUi->formatIndentMorePushButton, QNSIGNAL(QPushButton,released),
                     this, QNSLOT(NoteEditorWidget,onEditorTextIncreaseIndentationAction));
    QObject::connect(m_pUi->formatIndentLessPushButton, QNSIGNAL(QPushButton,released),
                     this, QNSLOT(NoteEditorWidget,onEditorTextDecreaseIndentationAction));
    QObject::connect(m_pUi->formatListUnorderedPushButton, QNSIGNAL(QPushButton,released),
                     this, QNSLOT(NoteEditorWidget,onEditorTextInsertUnorderedListAction));
    QObject::connect(m_pUi->formatListOrderedPushButton, QNSIGNAL(QPushButton,released),
                     this, QNSLOT(NoteEditorWidget,onEditorTextInsertOrderedListAction));
    QObject::connect(m_pUi->chooseTextColorToolButton, QNSIGNAL(ColorPickerToolButton,colorSelected,QColor),
                     this, QNSLOT(NoteEditorWidget,onEditorChooseTextColor,QColor));
    QObject::connect(m_pUi->chooseBackgroundColorToolButton, QNSIGNAL(ColorPickerToolButton,colorSelected,QColor),
                     this, QNSLOT(NoteEditorWidget,onEditorChooseBackgroundColor,QColor));
    QObject::connect(m_pUi->spellCheckBox, QNSIGNAL(QCheckBox,stateChanged,int),
                     this, QNSLOT(NoteEditorWidget,onEditorSpellCheckStateChanged,int));
    QObject::connect(m_pUi->insertToDoCheckboxPushButton, QNSIGNAL(QPushButton,released),
                     this, QNSLOT(NoteEditorWidget,onEditorInsertToDoCheckBoxAction));
    QObject::connect(m_pUi->insertTableToolButton, QNSIGNAL(InsertTableToolButton,createdTable,int,int,double,bool),
                     this, QNSLOT(NoteEditorWidget,onEditorInsertTable,int,int,double,bool));

    // Connect toolbar button actions to editor slots
    QObject::connect(m_pUi->copyPushButton, QNSIGNAL(QPushButton,clicked),
                     m_pUi->noteEditor, QNSLOT(NoteEditor,copy));
    QObject::connect(m_pUi->cutPushButton, QNSIGNAL(QPushButton,clicked),
                     m_pUi->noteEditor, QNSLOT(NoteEditor,cut));
    QObject::connect(m_pUi->pastePushButton, QNSIGNAL(QPushButton,clicked),
                     m_pUi->noteEditor, QNSLOT(NoteEditor,paste));
    QObject::connect(m_pUi->undoPushButton, QNSIGNAL(QPushButton,clicked),
                     m_pUi->noteEditor, QNSLOT(NoteEditor,undo));
    QObject::connect(m_pUi->redoPushButton, QNSIGNAL(QPushButton,clicked),
                     m_pUi->noteEditor, QNSLOT(NoteEditor,redo));
}

void NoteEditorWidget::clear()
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::clear: note ") << (m_pCurrentNote ? m_pCurrentNote->localUid() : QStringLiteral("<null>")));

    m_pCurrentNote.reset(Q_NULLPTR);
    m_pCurrentNotebook.reset(Q_NULLPTR);
    m_pUi->noteEditor->clear();
    m_pUi->tagNameLabelsContainer->clear();
    m_pUi->noteNameLineEdit->clear();

    m_lastNoteTitleOrPreviewText.clear();

    m_findCurrentNoteRequestId = QUuid();
    m_findCurrentNotebookRequestId = QUuid();
    m_updateNoteRequestIds.clear();

    m_pendingEditorSpellChecker = false;
    m_currentNoteWasExpunged = false;
}

void NoteEditorWidget::updateNoteSourceView(const QString & html)
{
    m_pUi->noteSourceView->setPlainText(html);
}

void NoteEditorWidget::setNoteAndNotebook(const Note & note, const Notebook & notebook)
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::setCurrentNoteAndNotebook"));
    QNTRACE(QStringLiteral("Note: ") << note << QStringLiteral("\nNotebook: ") << notebook);

    m_pUi->noteNameLineEdit->show();
    m_pUi->tagNameLabelsContainer->show();

    if (!m_noteTitleIsEdited && note.hasTitle())
    {
        QString title = note.title();
        m_pUi->noteNameLineEdit->setText(title);
        if (m_lastNoteTitleOrPreviewText != title) {
            m_lastNoteTitleOrPreviewText = title;
            emit titleOrPreviewChanged(m_lastNoteTitleOrPreviewText);
        }
    }
    else if (!m_noteTitleIsEdited)
    {
        m_pUi->noteNameLineEdit->clear();

        QString previewText;
        if (note.hasContent()) {
            previewText = note.plainText();
            previewText.truncate(140);
        }

        if (previewText != m_lastNoteTitleOrPreviewText) {
            m_lastNoteTitleOrPreviewText = previewText;
            emit titleOrPreviewChanged(m_lastNoteTitleOrPreviewText);
        }
    }

    m_pUi->noteEditor->setNoteAndNotebook(note, notebook);
    m_pUi->tagNameLabelsContainer->setCurrentNoteAndNotebook(note, notebook);
}

QString NoteEditorWidget::blankPageHtml() const
{
    QString html = QStringLiteral("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"http://www.w3.org/TR/html4/strict.dtd\">"
                                  "<html><head>"
                                  "<meta http-equiv=\"Content-Type\" content=\"text/html\" charset=\"UTF-8\" />"
                                  "<style>"
                                  "body {"
                                  "background-color: ");

    QColor backgroundColor = palette().color(QPalette::Window).darker(115);
    html += backgroundColor.name();

    html += QStringLiteral(";"
                           "color: ");
    QColor foregroundColor = palette().color(QPalette::WindowText);
    html += foregroundColor.name();

    html += QStringLiteral(";"
                           "-webkit-user-select: none;"
                           "}"
                           ".outer {"
                           "    display: table;"
                           "    position: absolute;"
                           "    height: 95%;"
                           "    width: 95%;"
                           "}"
                           ".middle {"
                           "    display: table-cell;"
                           "    vertical-align: middle;"
                           "}"
                           ".inner {"
                           "    text-align: center;"
                           "}"
                           "</style><title></title></head>"
                           "<body><div class=\"outer\"><div class=\"middle\"><div class=\"inner\">\n\n\n");

    html += tr("Please create a new note to start editing");

    html += QStringLiteral("</div></div></div></body></html>");

    return html;
}

void NoteEditorWidget::setupBlankEditor()
{
    QNDEBUG(QStringLiteral("NoteEditorWidget::setupBlankEditor"));

    m_pUi->noteNameLineEdit->hide();
    m_pUi->tagNameLabelsContainer->hide();

    QString initialHtml = blankPageHtml();
    m_pUi->noteEditor->setBlankPageHtml(initialHtml);

    m_pUi->findAndReplaceWidget->setHidden(true);
    m_pUi->noteSourceView->setHidden(true);
}

} // namespace quentier

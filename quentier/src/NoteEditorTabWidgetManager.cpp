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

#include "NoteEditorTabWidgetManager.h"
#include "models/TagModel.h"
#include "widgets/NoteEditorWidget.h"
#include "widgets/TabWidget.h"
#include <quentier/utility/ApplicationSettings.h>
#include <quentier/logging/QuentierLogger.h>
#include <quentier/utility/DesktopServices.h>
#include <QUndoStack>
#include <QTabBar>
#include <QCloseEvent>
#include <algorithm>

#define DEFAULT_MAX_NUM_NOTES (5)
#define BLANK_NOTE_KEY QStringLiteral("BlankNoteId")
#define MAX_TAB_NAME_SIZE (10)

namespace quentier {

NoteEditorTabWidgetManager::NoteEditorTabWidgetManager(const Account & account, LocalStorageManagerThreadWorker & localStorageWorker,
                                                       NoteCache & noteCache, NotebookCache & notebookCache,
                                                       TagCache & tagCache, TagModel & tagModel,
                                                       TabWidget * tabWidget, QObject * parent) :
    QObject(parent),
    m_currentAccount(account),
    m_localStorageWorker(localStorageWorker),
    m_noteCache(noteCache),
    m_notebookCache(notebookCache),
    m_tagCache(tagCache),
    m_tagModel(tagModel),
    m_pTabWidget(tabWidget),
    m_pBlankNoteEditor(Q_NULLPTR),
    m_maxNumNotes(-1),
    m_shownNoteLocalUids(),
    m_createNoteRequestIds()
{
    ApplicationSettings appSettings;
    appSettings.beginGroup(QStringLiteral("NoteEditor"));
    QVariant maxNumNoteTabsData = appSettings.value(QStringLiteral("MaxNumNoteTabs"));
    appSettings.endGroup();

    bool conversionResult = false;
    int maxNumNoteTabs = maxNumNoteTabsData.toInt(&conversionResult);
    if (!conversionResult) {
        QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager: no persisted max num note tabs setting, "
                               "fallback to the default value of ") << DEFAULT_MAX_NUM_NOTES);
        m_maxNumNotes = DEFAULT_MAX_NUM_NOTES;
    }
    else {
        QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager: max num note tabs: ") << maxNumNoteTabs);
        m_maxNumNotes = maxNumNoteTabs;
    }

    QUndoStack * pUndoStack = new QUndoStack;
    m_pBlankNoteEditor = new NoteEditorWidget(m_currentAccount, m_localStorageWorker,
                                              m_noteCache, m_notebookCache, m_tagCache,
                                              m_tagModel, pUndoStack, m_pTabWidget);
    m_pBlankNoteEditor->installEventFilter(this);
    pUndoStack->setParent(m_pBlankNoteEditor);

    Q_UNUSED(m_pTabWidget->addTab(m_pBlankNoteEditor, BLANK_NOTE_KEY))

    m_pTabWidget->tabBar()->hide();

    QObject::connect(m_pTabWidget, QNSIGNAL(TabWidget,tabCloseRequested,int),
                     this, QNSLOT(NoteEditorTabWidgetManager,onNoteEditorTabCloseRequested,int));
}

void NoteEditorTabWidgetManager::setMaxNumNotes(const int maxNumNotes)
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::setMaxNumNotes: ") << maxNumNotes);

    if (m_maxNumNotes == maxNumNotes) {
        QNDEBUG(QStringLiteral("Max number of notes hasn't changed"));
        return;
    }

    if (m_maxNumNotes < maxNumNotes) {
        m_maxNumNotes = maxNumNotes;
        QNDEBUG(QStringLiteral("Max number of notes has been increased to ") << maxNumNotes);
        return;
    }

    int currentNumNotes = numNotes();

    m_maxNumNotes = maxNumNotes;
    QNDEBUG(QStringLiteral("Max number of notes has been decreased to ") << maxNumNotes);

    if (currentNumNotes <= maxNumNotes) {
        return;
    }

    m_shownNoteLocalUids.set_capacity(static_cast<size_t>(std::max(maxNumNotes, 0)));
    checkAndCloseOlderNoteEditors();
}

int NoteEditorTabWidgetManager::numNotes() const
{
    if (Q_UNLIKELY(!m_pTabWidget)) {
        return 0;
    }

    if (m_pBlankNoteEditor) {
        return std::max(m_pTabWidget->count() - 1, 0);
    }
    else {
        return m_pTabWidget->count();
    }
}

void NoteEditorTabWidgetManager::addNote(const QString & noteLocalUid)
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::addNote: ") << noteLocalUid);

    // First, check if this note is already open in some existing tab
    for(auto it = m_shownNoteLocalUids.begin(), end = m_shownNoteLocalUids.end(); it != end; ++it)
    {
        const QString & existingNoteLocalUid = *it;
        if (Q_UNLIKELY(existingNoteLocalUid == noteLocalUid)) {
            QNDEBUG(QStringLiteral("The note attempted to be added to the note editor tab widget has already been added to it"));
            return;
        }
    }

    if (m_shownNoteLocalUids.empty() && m_pBlankNoteEditor)
    {
        QNDEBUG(QStringLiteral("Currently only the blank note tab is displayed, inserting the new note into its editor"));
        // There's only the blank note's note editor displayed, just set the note into it
        NoteEditorWidget * pNoteEditorWidget = m_pBlankNoteEditor;
        m_pBlankNoteEditor = Q_NULLPTR;

        pNoteEditorWidget->setNoteLocalUid(noteLocalUid);

        insertNoteEditorWidget(pNoteEditorWidget);
        return;
    }

    QUndoStack * pUndoStack = new QUndoStack;
    NoteEditorWidget * pNoteEditorWidget = new NoteEditorWidget(m_currentAccount, m_localStorageWorker,
                                                                m_noteCache, m_notebookCache, m_tagCache,
                                                                m_tagModel, pUndoStack, m_pTabWidget);
    pUndoStack->setParent(pNoteEditorWidget);

    pNoteEditorWidget->setNoteLocalUid(noteLocalUid);
    pNoteEditorWidget->installEventFilter(this);

    insertNoteEditorWidget(pNoteEditorWidget);
}

void NoteEditorTabWidgetManager::createNewNote(const QString & notebookLocalUid, const QString & notebookGuid)
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::createNewNote: notebook local uid = ")
            << notebookLocalUid << QStringLiteral(", notebook guid = ") << notebookGuid);

    Note newNote;
    newNote.setNotebookLocalUid(notebookLocalUid);
    newNote.setLocal(m_currentAccount.type() == Account::Type::Local);
    newNote.setDirty(true);
    newNote.setContent(QStringLiteral("<en-note><div></div></en-note>"));

    if (!notebookGuid.isEmpty()) {
        newNote.setNotebookGuid(notebookGuid);
    }

    connectToLocalStorage();

    QUuid requestId = QUuid::createUuid();
    Q_UNUSED(m_createNoteRequestIds.insert(requestId))
    QNTRACE(QStringLiteral("Emitting the request to add a new note to the local storage: request id = ")
            << requestId << QStringLiteral(", note = ") << newNote);
    emit requestAddNote(newNote, requestId);
}

void NoteEditorTabWidgetManager::onNoteEditorWidgetResolved()
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::onNoteEditorWidgetResolved"));

    NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(sender());
    if (Q_UNLIKELY(!pNoteEditorWidget)) {
        QNLocalizedString error = QT_TR_NOOP("Internal error: can't resolve the added note editor, cast failed");
        QNWARNING(error);
        emit notifyError(error);
        return;
    }

    QObject::disconnect(pNoteEditorWidget, QNSIGNAL(NoteEditorWidget,resolved),
                        this, QNSLOT(NoteEditorTabWidgetManager,onNoteEditorWidgetResolved));

    insertNoteEditorWidget(pNoteEditorWidget);
}

void NoteEditorTabWidgetManager::onNoteTitleOrPreviewTextChanged(QString titleOrPreview)
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::onNoteTitleOrPreviewTextChanged: ") << titleOrPreview);

    NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(sender());
    if (Q_UNLIKELY(!pNoteEditorWidget)) {
        QNLocalizedString error = QT_TR_NOOP("Internal error: can't update the note editor's tab name, cast failed");
        QNWARNING(error);
        emit notifyError(error);
        return;
    }

    for(int i = 0, numTabs = m_pTabWidget->count(); i < numTabs; ++i)
    {
        if (pNoteEditorWidget != m_pTabWidget->widget(i)) {
            continue;
        }

        QString tabName = shortenTabName(titleOrPreview);
        m_pTabWidget->setTabText(i, tabName);
        return;
    }

    QNLocalizedString error = QT_TR_NOOP("Internal error: can't find the note editor which has sent the title or preview text update");
    QNWARNING(error);
    emit notifyError(error);
}

void NoteEditorTabWidgetManager::onNoteEditorTabCloseRequested(int tabIndex)
{
    if (m_pTabWidget->count() == 1)
    {
        // We shouldn't have ended up here really but if we have, let's try to play it nice
        NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(m_pTabWidget->widget(tabIndex));
        if (Q_UNLIKELY(!pNoteEditorWidget)) {
            QNWARNING(QStringLiteral("Detected attempt to close the note editor tab but can't cast the tab widget's tab to note editor"));
            return;
        }

        QNLocalizedString errorDescription;
        NoteEditorWidget::NoteSaveStatus::type status = pNoteEditorWidget->checkAndSaveModifiedNote(errorDescription);
        QNDEBUG(QStringLiteral("Check and save modified note, status: ") << status
                << QStringLiteral(", error description: ") << errorDescription);

        // That should remove the note from the editor (if any)
        pNoteEditorWidget->setNoteLocalUid(QString());
        return;
    }

    m_pTabWidget->removeTab(tabIndex);
}

void NoteEditorTabWidgetManager::onNoteLoadedInEditor()
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::onNoteLoadedInEditor"));

    NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(sender());
    if (Q_UNLIKELY(!pNoteEditorWidget)) {
        QNWARNING(QStringLiteral("Failed to cast the sender of note loaded signal to NoteEditorWidget"));
        return;
    }

    if (pNoteEditorWidget->isNoteTitleEdited()) {
        pNoteEditorWidget->setFocusToTitle();
    }
    else {
        pNoteEditorWidget->setFocusToEditor();
    }
}

void NoteEditorTabWidgetManager::onAddNoteComplete(Note note, QUuid requestId)
{
    auto it = m_createNoteRequestIds.find(requestId);
    if (it == m_createNoteRequestIds.end()) {
        return;
    }

    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::onAddNoteComplete: request id = ")
            << requestId << QStringLiteral(", note: ") << note);

    Q_UNUSED(m_createNoteRequestIds.erase(it))
    disconnectFromLocalStorage();

    m_noteCache.put(note.localUid(), note);
    addNote(note.localUid());
}

void NoteEditorTabWidgetManager::onAddNoteFailed(Note note, QNLocalizedString errorDescription, QUuid requestId)
{
    auto it = m_createNoteRequestIds.find(requestId);
    if (it == m_createNoteRequestIds.end()) {
        return;
    }

    QNWARNING(QStringLiteral("NoteEditorTabWidgetManager::onAddNoteFailed: request id = ")
              << requestId << QStringLiteral(", note: ") << note << QStringLiteral("\nError description: ")
              << errorDescription);

    Q_UNUSED(m_createNoteRequestIds.erase(it))
    disconnectFromLocalStorage();

    Q_UNUSED(internalErrorMessageBox(m_pTabWidget,
                                     tr("Note creation in local storage has failed") +
                                     QStringLiteral(": ") + errorDescription.localizedString()))
}

void NoteEditorTabWidgetManager::insertNoteEditorWidget(NoteEditorWidget * pNoteEditorWidget)
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::insertNoteEditorWidget: ") << pNoteEditorWidget->noteLocalUid());

    QObject::connect(pNoteEditorWidget, QNSIGNAL(NoteEditorWidget,titleOrPreviewChanged,QString),
                     this, QNSLOT(NoteEditorTabWidgetManager,onNoteTitleOrPreviewTextChanged,QString));
    QObject::connect(pNoteEditorWidget, QNSIGNAL(NoteEditorWidget,resolved),
                     this, QNSLOT(NoteEditorTabWidgetManager,onNoteEditorWidgetResolved));
    QObject::connect(pNoteEditorWidget, QNSIGNAL(NoteEditorWidget,noteLoaded),
                     this, QNSLOT(NoteEditorTabWidgetManager,onNoteLoadedInEditor));

    QString tabName = shortenTabName(pNoteEditorWidget->titleOrPreview());
    int tabIndex = m_pTabWidget->addTab(pNoteEditorWidget, tabName);
    m_pTabWidget->setCurrentIndex(tabIndex);

    m_shownNoteLocalUids.push_back(pNoteEditorWidget->noteLocalUid());

    int currentNumNotes = numNotes();
    if (currentNumNotes <= m_maxNumNotes)
    {
        QNDEBUG(QStringLiteral("The addition of note ") << pNoteEditorWidget->noteLocalUid()
                << QStringLiteral(" doesn't cause the overflow of max allowed number of note editor tabs"));

        if (currentNumNotes >= 1) {
            m_pTabWidget->tabBar()->show();
            m_pTabWidget->setTabsClosable(true);
        }
        else {
            m_pTabWidget->tabBar()->hide();
            m_pTabWidget->setTabsClosable(false);
        }

        return;
    }

    checkAndCloseOlderNoteEditors();
}

void NoteEditorTabWidgetManager::checkAndCloseOlderNoteEditors()
{
    for(int i = 0; i < m_pTabWidget->count(); ++i)
    {
        NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(m_pTabWidget->widget(i));
        if (Q_UNLIKELY(!pNoteEditorWidget)) {
            continue;
        }

        if (Q_UNLIKELY(pNoteEditorWidget == m_pBlankNoteEditor)) {
            continue;
        }

        const QString & noteLocalUid = pNoteEditorWidget->noteLocalUid();
        auto it = std::find(m_shownNoteLocalUids.begin(), m_shownNoteLocalUids.end(), noteLocalUid);
        if (it == m_shownNoteLocalUids.end()) {
            m_pTabWidget->removeTab(i);
            Q_UNUSED(pNoteEditorWidget->close())
        }
    }

    if (m_pTabWidget->count() <= 1) {
        m_pTabWidget->tabBar()->hide();
        m_pTabWidget->setTabsClosable(false);
    }
    else {
        m_pTabWidget->tabBar()->show();
        m_pTabWidget->setTabsClosable(true);
    }
}

bool NoteEditorTabWidgetManager::eventFilter(QObject * pWatched, QEvent * pEvent)
{
    if (Q_UNLIKELY(!pWatched)) {
        QNWARNING(QStringLiteral("NoteEditorTabWidgetManager: caught event for null watched object in the eventFilter method"));
        return true;
    }

    if (Q_UNLIKELY(!pEvent)) {
        QNWARNING(QStringLiteral("NoteEditorTabWidgetManager: caught null event in the eventFilter method for object ")
                  << pWatched->objectName() << QStringLiteral(" of class ") << pWatched->metaObject()->className());
        return true;
    }

    NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(pWatched);
    if (Q_UNLIKELY(!pNoteEditorWidget)) {
        return false;
    }

    if (pEvent->type() != QEvent::Close) {
        return false;
    }

    int numNoteEditorWidgets = m_pTabWidget->count();
    if (numNoteEditorWidgets == 2) {
        // There are two editors, one is about to close, so the only one would be left, need to hide the close button from its tab
        m_pTabWidget->setTabsClosable(false);
        return false;
    }

    if (numNoteEditorWidgets == 1)
    {
        // We shouldn't have got here (see just above) but if we are here anyway, need to clear the note from the editor widget
        // but leave the only remaining editor widget

        QNLocalizedString errorDescription;
        NoteEditorWidget::NoteSaveStatus::type status = pNoteEditorWidget->checkAndSaveModifiedNote(errorDescription);
        QNDEBUG(QStringLiteral("Check and save modified note, status: ") << status
                << QStringLiteral(", error description: ") << errorDescription);

        pNoteEditorWidget->setNoteLocalUid(QString());
        return true;
    }

    return false;
}

void NoteEditorTabWidgetManager::connectToLocalStorage()
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::connectToLocalStorage"));

    QObject::connect(this, QNSIGNAL(NoteEditorTabWidgetManager,requestAddNote,Note,QUuid),
                     &m_localStorageWorker, QNSLOT(LocalStorageManagerThreadWorker,onAddNoteRequest,Note,QUuid));
    QObject::connect(&m_localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,addNoteComplete,Note,QUuid),
                     this, QNSLOT(NoteEditorTabWidgetManager,onAddNoteComplete,Note,QUuid));
    QObject::connect(&m_localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,addNoteFailed,Note,QNLocalizedString,QUuid),
                     this, QNSLOT(NoteEditorTabWidgetManager,onAddNoteFailed,Note,QNLocalizedString,QUuid));
}

void NoteEditorTabWidgetManager::disconnectFromLocalStorage()
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::disconnectFromLocalStorage"));

    QObject::disconnect(this, QNSIGNAL(NoteEditorTabWidgetManager,requestAddNote,Note,QUuid),
                        &m_localStorageWorker, QNSLOT(LocalStorageManagerThreadWorker,onAddNoteRequest,Note,QUuid));
    QObject::disconnect(&m_localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,addNoteComplete,Note,QUuid),
                        this, QNSLOT(NoteEditorTabWidgetManager,onAddNoteComplete,Note,QUuid));
    QObject::disconnect(&m_localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,addNoteFailed,Note,QNLocalizedString,QUuid),
                        this, QNSLOT(NoteEditorTabWidgetManager,onAddNoteFailed,Note,QNLocalizedString,QUuid));
}

QString NoteEditorTabWidgetManager::shortenTabName(const QString & tabName) const
{
    if (tabName.size() <= MAX_TAB_NAME_SIZE) {
        return tabName;
    }

    QString result = tabName;
    result.truncate(std::max(MAX_TAB_NAME_SIZE - 3, 0));
    result += QStringLiteral("...");
    return result;
}

} // namespace quentier

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
#include <quentier/logging/QuentierLogger.h>
#include <quentier/utility/ApplicationSettings.h>
#include <quentier/utility/DesktopServices.h>
#include <quentier/utility/FileIOThreadWorker.h>
#include <quentier/note_editor/SpellChecker.h>
#include <QUndoStack>
#include <QTabBar>
#include <QCloseEvent>
#include <QThread>
#include <QMenu>
#include <QContextMenuEvent>
#include <algorithm>

#define DEFAULT_MAX_NUM_NOTES_IN_TABS (5)
#define MIN_NUM_NOTES_IN_TABS (1)

#define BLANK_NOTE_KEY QStringLiteral("BlankNoteId")
#define MAX_TAB_NAME_SIZE (10)

#define OPEN_NOTES_LOCAL_UIDS_SETTINGS_KEY QStringLiteral("LocalUidsOfNotesLastOpenInNoteEditorTabs")

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
    m_pIOThread(Q_NULLPTR),
    m_pFileIOThreadWorker(Q_NULLPTR),
    m_pSpellChecker(Q_NULLPTR),
    m_maxNumNotesInTabs(-1),
    m_localUidsOfNotesInTabbedEditors(),
    m_lastCurrentNoteLocalUid(),
    m_createNoteRequestIds(),
    m_pTabBarContextMenu(Q_NULLPTR)
{
    ApplicationSettings appSettings;
    appSettings.beginGroup(QStringLiteral("NoteEditor"));
    QVariant maxNumNoteTabsData = appSettings.value(QStringLiteral("MaxNumNoteTabs"));
    appSettings.endGroup();

    bool conversionResult = false;
    int maxNumNoteTabs = maxNumNoteTabsData.toInt(&conversionResult);
    if (!conversionResult) {
        QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager: no persisted max num note tabs setting, "
                               "fallback to the default value of ") << DEFAULT_MAX_NUM_NOTES_IN_TABS);
        m_maxNumNotesInTabs = DEFAULT_MAX_NUM_NOTES_IN_TABS;
    }
    else {
        QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager: max num note tabs: ") << maxNumNoteTabs);
        m_maxNumNotesInTabs = maxNumNoteTabs;
    }

    m_localUidsOfNotesInTabbedEditors.set_capacity(static_cast<size_t>(std::max(m_maxNumNotesInTabs, MIN_NUM_NOTES_IN_TABS)));
    QNTRACE(QStringLiteral("Tabbed note local uids circular buffer capacity: ") << m_localUidsOfNotesInTabbedEditors.capacity());

    setupFileIO();
    setupSpellChecker();

    QUndoStack * pUndoStack = new QUndoStack;
    m_pBlankNoteEditor = new NoteEditorWidget(m_currentAccount, m_localStorageWorker,
                                              *m_pFileIOThreadWorker, *m_pSpellChecker,
                                              m_noteCache, m_notebookCache, m_tagCache,
                                              m_tagModel, pUndoStack, m_pTabWidget);
    pUndoStack->setParent(m_pBlankNoteEditor);

    Q_UNUSED(m_pTabWidget->addTab(m_pBlankNoteEditor, BLANK_NOTE_KEY))

    QTabBar * pTabBar = m_pTabWidget->tabBar();
    pTabBar->hide();
    pTabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(pTabBar, QNSIGNAL(QTabBar,customContextMenuRequested,QPoint),
                     this, QNSLOT(NoteEditorTabWidgetManager,onTabContextMenuRequested,QPoint));

    QObject::connect(m_pTabWidget, QNSIGNAL(TabWidget,tabCloseRequested,int),
                     this, QNSLOT(NoteEditorTabWidgetManager,onNoteEditorTabCloseRequested,int));
    QObject::connect(m_pTabWidget, QNSIGNAL(TabWidget,currentChanged,int),
                     this, QNSLOT(NoteEditorTabWidgetManager,onCurrentTabChanged,int));

    restoreLastOpenNotes();
}

NoteEditorTabWidgetManager::~NoteEditorTabWidgetManager()
{
    if (m_pIOThread) {
        m_pIOThread->quit();
    }
}

void NoteEditorTabWidgetManager::setMaxNumNotesInTabs(const int maxNumNotesInTabs)
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::setMaxNumNotesInTabs: ") << maxNumNotesInTabs);

    if (m_maxNumNotesInTabs == maxNumNotesInTabs) {
        QNDEBUG(QStringLiteral("Max number of notes in tabs hasn't changed"));
        return;
    }

    if (m_maxNumNotesInTabs < maxNumNotesInTabs) {
        m_maxNumNotesInTabs = maxNumNotesInTabs;
        QNDEBUG(QStringLiteral("Max number of notes in tabs has been increased to ") << maxNumNotesInTabs);
        return;
    }

    int currentNumNotesInTabs = numNotesInTabs();

    m_maxNumNotesInTabs = maxNumNotesInTabs;
    QNDEBUG(QStringLiteral("Max number of notes in tabs has been decreased to ") << maxNumNotesInTabs);

    if (currentNumNotesInTabs <= maxNumNotesInTabs) {
        return;
    }

    m_localUidsOfNotesInTabbedEditors.set_capacity(static_cast<size_t>(std::max(maxNumNotesInTabs, MIN_NUM_NOTES_IN_TABS)));
    QNTRACE(QStringLiteral("Tabbed note local uids circular buffer capacity: ") << m_localUidsOfNotesInTabbedEditors.capacity());
    checkAndCloseOlderNoteEditorTabs();
}

int NoteEditorTabWidgetManager::numNotesInTabs() const
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
    for(auto it = m_localUidsOfNotesInTabbedEditors.begin(), end = m_localUidsOfNotesInTabbedEditors.end(); it != end; ++it)
    {
        QNTRACE(QStringLiteral("Examining tabbed note local uid = ") << *it);

        const QString & existingNoteLocalUid = *it;
        if (Q_UNLIKELY(existingNoteLocalUid == noteLocalUid)) {
            QNDEBUG(QStringLiteral("The note attempted to be added to the note editor tab widget has already been added to it, "
                                   "making it the current one"));
            setCurrentNoteEditorWidget(noteLocalUid);
            return;
        }
    }

    if (m_localUidsOfNotesInTabbedEditors.empty() && m_pBlankNoteEditor)
    {
        QNDEBUG(QStringLiteral("Currently only the blank note tab is displayed, inserting the new note into its editor"));
        // There's only the blank note's note editor displayed, just set the note into it
        NoteEditorWidget * pNoteEditorWidget = m_pBlankNoteEditor;
        m_pBlankNoteEditor = Q_NULLPTR;

        pNoteEditorWidget->setNoteLocalUid(noteLocalUid);
        insertNoteEditorWidget(pNoteEditorWidget);

        m_lastCurrentNoteLocalUid = noteLocalUid;
        QNTRACE(QStringLiteral("Emitting the update of last current note local uid to ")
                << m_lastCurrentNoteLocalUid);
        emit currentNoteChanged(m_lastCurrentNoteLocalUid);
        return;
    }

    QUndoStack * pUndoStack = new QUndoStack;
    NoteEditorWidget * pNoteEditorWidget = new NoteEditorWidget(m_currentAccount, m_localStorageWorker,
                                                                *m_pFileIOThreadWorker, *m_pSpellChecker,
                                                                m_noteCache, m_notebookCache, m_tagCache,
                                                                m_tagModel, pUndoStack, m_pTabWidget);
    pUndoStack->setParent(pNoteEditorWidget);
    pNoteEditorWidget->setNoteLocalUid(noteLocalUid);
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

    qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
    newNote.setCreationTimestamp(timestamp);
    newNote.setModificationTimestamp(timestamp);

    QString sourceApplicationName = QApplication::applicationName();
    int sourceApplicationNameSize = sourceApplicationName.size();
    if ((sourceApplicationNameSize >= qevercloud::EDAM_ATTRIBUTE_LEN_MIN) &&
        (sourceApplicationNameSize <= qevercloud::EDAM_ATTRIBUTE_LEN_MAX))
    {
        qevercloud::NoteAttributes & noteAttributes = newNote.noteAttributes();
        noteAttributes.sourceApplication = sourceApplicationName;
    }

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
        ErrorString error(QT_TRANSLATE_NOOP("", "Internal error: can't resolve the added note editor, cast failed"));
        QNWARNING(error);
        emit notifyError(error);
        return;
    }

    QObject::disconnect(pNoteEditorWidget, QNSIGNAL(NoteEditorWidget,resolved),
                        this, QNSLOT(NoteEditorTabWidgetManager,onNoteEditorWidgetResolved));

    int tabIndex = -1;
    for(int i = 0, size = m_pTabWidget->count(); i < size; ++i)
    {
        NoteEditorWidget * pCurrentNoteEditorWidget = qobject_cast<NoteEditorWidget*>(m_pTabWidget->widget(i));
        if (!pCurrentNoteEditorWidget) {
            continue;
        }

        if (pCurrentNoteEditorWidget == pNoteEditorWidget) {
            tabIndex = i;
            break;
        }
    }

    if (Q_UNLIKELY(tabIndex < 0)) {
        QNWARNING(QStringLiteral("Couldn't find the resolved note editor widget within tabs: ")
                  << pNoteEditorWidget->noteLocalUid());
        return;
    }

    QString tabName = shortenTabName(pNoteEditorWidget->titleOrPreview());
    m_pTabWidget->setTabText(tabIndex, tabName);
}

void NoteEditorTabWidgetManager::onNoteEditorWidgetInvalidated()
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::onNoteEditorWidgetInvalidated"));

    NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(sender());
    if (Q_UNLIKELY(!pNoteEditorWidget)) {
        ErrorString error(QT_TRANSLATE_NOOP("", "Internal error: can't invalidate the note editor, cast failed"));
        QNWARNING(error);
        emit notifyError(error);
        return;
    }

    for(int i = 0, numTabs = m_pTabWidget->count(); i < numTabs; ++i)
    {
        if (pNoteEditorWidget != m_pTabWidget->widget(i)) {
            continue;
        }

        onNoteEditorTabCloseRequested(i);
        break;
    }
}

void NoteEditorTabWidgetManager::onNoteTitleOrPreviewTextChanged(QString titleOrPreview)
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::onNoteTitleOrPreviewTextChanged: ") << titleOrPreview);

    NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(sender());
    if (Q_UNLIKELY(!pNoteEditorWidget)) {
        ErrorString error(QT_TRANSLATE_NOOP("", "Internal error: can't update the note editor's tab name, cast failed"));
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

    ErrorString error(QT_TRANSLATE_NOOP("", "Internal error: can't find the note editor which has sent "
                                        "the title or preview text update"));
    QNWARNING(error);
    emit notifyError(error);
}

void NoteEditorTabWidgetManager::onNoteEditorTabCloseRequested(int tabIndex)
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::onNoteEditorTabCloseRequested: ") << tabIndex);

    NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(m_pTabWidget->widget(tabIndex));
    if (Q_UNLIKELY(!pNoteEditorWidget)) {
        QNWARNING(QStringLiteral("Detected attempt to close the note editor tab but can't cast the tab widget's tab to note editor"));
        return;
    }

    if (pNoteEditorWidget == m_pBlankNoteEditor) {
        QNDEBUG(QStringLiteral("Silently refusing to remove the blank note editor tab"));
        return;
    }

    ErrorString errorDescription;
    NoteEditorWidget::NoteSaveStatus::type status = pNoteEditorWidget->checkAndSaveModifiedNote(errorDescription);
    QNDEBUG(QStringLiteral("Check and save modified note, status: ") << status
            << QStringLiteral(", error description: ") << errorDescription);

    QString noteLocalUid = pNoteEditorWidget->noteLocalUid();

    auto it = std::find(m_localUidsOfNotesInTabbedEditors.begin(), m_localUidsOfNotesInTabbedEditors.end(), noteLocalUid);
    if (it != m_localUidsOfNotesInTabbedEditors.end()) {
        Q_UNUSED(m_localUidsOfNotesInTabbedEditors.erase(it))
        QNTRACE(QStringLiteral("Removed note local uid ") << pNoteEditorWidget->noteLocalUid());
        persistLocalUidsOfOpenNotes();
    }

    QStringLiteral("Remaining tabbed note local uids: ");
    for(auto sit = m_localUidsOfNotesInTabbedEditors.begin(), end = m_localUidsOfNotesInTabbedEditors.end(); sit != end; ++sit) {
        QNTRACE(*sit);
    }

    if (m_lastCurrentNoteLocalUid == noteLocalUid) {
        m_lastCurrentNoteLocalUid.clear();
        QNTRACE(QStringLiteral("Emitting last current note local uid update to empty"));
        emit currentNoteChanged(QString());
    }

    if (m_pTabWidget->count() == 1)
    {
        if (m_pBlankNoteEditor) {
            m_pBlankNoteEditor->close();
            m_pBlankNoteEditor = Q_NULLPTR;
        }

        // That should remove the note from the editor (if any)
        pNoteEditorWidget->setNoteLocalUid(QString());
        m_pBlankNoteEditor = pNoteEditorWidget;
        m_pTabWidget->setTabText(0, BLANK_NOTE_KEY);
        m_pTabWidget->tabBar()->hide();
        m_pTabWidget->setTabsClosable(false);

        return;
    }

    m_pTabWidget->removeTab(tabIndex);
    Q_UNUSED(pNoteEditorWidget->close());

    if (m_pTabWidget->count() == 1) {
        m_pTabWidget->tabBar()->hide();
        m_pTabWidget->setTabsClosable(false);
    }
}

void NoteEditorTabWidgetManager::onNoteLoadedInEditor()
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::onNoteLoadedInEditor"));
}

void NoteEditorTabWidgetManager::onNoteEditorError(ErrorString errorDescription)
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::onNoteEditorError: ") << errorDescription);

    NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(sender());
    if (Q_UNLIKELY(!pNoteEditorWidget)) {
        QNWARNING(QStringLiteral("Received error from note editor but can't cast the sender to NoteEditorWidget; error: ")
                  << errorDescription);
        emit notifyError(errorDescription);
        return;
    }

    ErrorString error(QT_TRANSLATE_NOOP("", "Note editor error"));
    error.additionalBases().append(errorDescription.base());
    error.additionalBases().append(errorDescription.additionalBases());
    error.details() = errorDescription.details();

    QString titleOrPreview = pNoteEditorWidget->titleOrPreview();
    if (Q_UNLIKELY(titleOrPreview.isEmpty())) {
        error.details() += QStringLiteral(", note local uid ");
        error.details() += pNoteEditorWidget->noteLocalUid();
    }
    else {
        error.details() = QStringLiteral("note \"");
        error.details() += titleOrPreview;
        error.details() += QStringLiteral("\"");
    }

    emit notifyError(error);
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

void NoteEditorTabWidgetManager::onAddNoteFailed(Note note, ErrorString errorDescription, QUuid requestId)
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

void NoteEditorTabWidgetManager::onCurrentTabChanged(int currentIndex)
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::onCurrentTabChanged: ") << currentIndex);

    if (currentIndex < 0)
    {
        if (!m_lastCurrentNoteLocalUid.isEmpty()) {
            m_lastCurrentNoteLocalUid.clear();
            QNTRACE(QStringLiteral("Emitting last current note local uid update to empty"));
            emit currentNoteChanged(QString());
        }

        return;
    }

    NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(m_pTabWidget->widget(currentIndex));
    if (Q_UNLIKELY(!pNoteEditorWidget))
    {
        QNWARNING(QStringLiteral("Detected current tab change in the note editor tab widget "
                                 "but can't cast the tab widget's tab to note editor"));

        if (!m_lastCurrentNoteLocalUid.isEmpty()) {
            m_lastCurrentNoteLocalUid.clear();
            QNTRACE(QStringLiteral("Emitting last current note local uid update to empty"));
            emit currentNoteChanged(QString());
        }

        return;
    }

    if (pNoteEditorWidget == m_pBlankNoteEditor)
    {
        QNTRACE(QStringLiteral("Switched to blank note editor"));

        if (!m_lastCurrentNoteLocalUid.isEmpty()) {
            m_lastCurrentNoteLocalUid.clear();
            QNTRACE(QStringLiteral("Emitting last current note local uid update to empty"));
            emit currentNoteChanged(QString());
        }

        return;
    }

    QString currentNoteLocalUid = pNoteEditorWidget->noteLocalUid();
    if (m_lastCurrentNoteLocalUid != currentNoteLocalUid) {
        m_lastCurrentNoteLocalUid = currentNoteLocalUid;
        QNTRACE(QStringLiteral("Emitting last current note local uid update to ") << m_lastCurrentNoteLocalUid);
        emit currentNoteChanged(currentNoteLocalUid);
    }
}

void NoteEditorTabWidgetManager::onTabContextMenuRequested(const QPoint & pos)
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::onTabContextMenuRequested, pos: x = ")
            << pos.x() << QStringLiteral(", y = ") << pos.y());

    int tabIndex = m_pTabWidget->tabBar()->tabAt(pos);
    if (Q_UNLIKELY(tabIndex < 0)) {
        ErrorString error(QT_TRANSLATE_NOOP("", "Can't show the tab context menu: can't find the tab index "
                                            "of the target note editor"));
        QNWARNING(error);
        emit notifyError(error);
        return;
    }

    NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(m_pTabWidget->widget(tabIndex));
    if (Q_UNLIKELY(!pNoteEditorWidget)) {
        ErrorString error(QT_TRANSLATE_NOOP("", "Can't show the tab context menu: can't cast the widget "
                                            "at the clicked tab to note editor"));
        QNWARNING(error << QStringLiteral(", tab index = ") << tabIndex);
        emit notifyError(error);
        return;
    }

    delete m_pTabBarContextMenu;
    m_pTabBarContextMenu = new QMenu(m_pTabWidget);

#define ADD_CONTEXT_MENU_ACTION(name, menu, slot, data, enabled) \
    { \
        QAction * pAction = new QAction(name, menu); \
        pAction->setData(data); \
        pAction->setEnabled(enabled); \
        QObject::connect(pAction, QNSIGNAL(QAction,triggered), this, QNSLOT(NoteEditorTabWidgetManager,slot)); \
        menu->addAction(pAction); \
    }

    if (pNoteEditorWidget->isModified()) {
        ADD_CONTEXT_MENU_ACTION(tr("Save"), m_pTabBarContextMenu, onTabContextMenuSaveNoteAction,
                                pNoteEditorWidget->noteLocalUid(), true);
    }

    ADD_CONTEXT_MENU_ACTION(tr("Close"), m_pTabBarContextMenu, onTabContextMenuCloseEditorAction,
                            pNoteEditorWidget->noteLocalUid(), true);

    m_pTabBarContextMenu->show();
    m_pTabBarContextMenu->exec(m_pTabWidget->tabBar()->mapToGlobal(pos));
}

void NoteEditorTabWidgetManager::onTabContextMenuCloseEditorAction()
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::onTabContextMenuCloseEditorAction"));

    QAction * pAction = qobject_cast<QAction*>(sender());
    if (Q_UNLIKELY(!pAction)) {
        ErrorString error(QT_TRANSLATE_NOOP("", "Internal error: can't close the chosen note editor, "
                                            "can't cast the slot invoker to QAction"));
        QNWARNING(error);
        emit notifyError(error);
        return;
    }

    QString noteLocalUid = pAction->data().toString();
    if (Q_UNLIKELY(noteLocalUid.isEmpty())) {
        ErrorString error(QT_TRANSLATE_NOOP("", "Internal error: can't close the chosen note editor, "
                                            "can't get the note local uid corresponding to the editor"));
        QNWARNING(error);
        emit notifyError(error);
        return;
    }

    for(int i = 0; i < m_pTabWidget->count(); ++i)
    {
        NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(m_pTabWidget->widget(i));
        if (Q_UNLIKELY(!pNoteEditorWidget)) {
            continue;
        }

        if (pNoteEditorWidget->noteLocalUid() == noteLocalUid) {
            onNoteEditorTabCloseRequested(i);
            return;
        }
    }

    // If we got here, no target note editor widget was found
    ErrorString error(QT_TRANSLATE_NOOP("", "Internal error: can't close the chosen note editor, "
                                        "can't find the editor to be closed by note local uid"));
    QNWARNING(error << QStringLiteral(", note local uid = ") << noteLocalUid);
    emit notifyError(error);
    return;
}

void NoteEditorTabWidgetManager::onTabContextMenuSaveNoteAction()
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::onTabContextMenuSaveNoteAction"));

    QAction * pAction = qobject_cast<QAction*>(sender());
    if (Q_UNLIKELY(!pAction)) {
        ErrorString error(QT_TRANSLATE_NOOP("", "Internal error: can't save the note within the chosen note editor, "
                                            "can't cast the slot invoker to QAction"));
        QNWARNING(error);
        emit notifyError(error);
        return;
    }

    QString noteLocalUid = pAction->data().toString();
    if (Q_UNLIKELY(noteLocalUid.isEmpty())) {
        ErrorString error(QT_TRANSLATE_NOOP("", "Internal error: can't save the note within the chosen note editor, "
                                            "can't get the note local uid corresponding to the editor"));
        QNWARNING(error);
        emit notifyError(error);
        return;
    }

    for(int i = 0; i < m_pTabWidget->count(); ++i)
    {
        NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(m_pTabWidget->widget(i));
        if (Q_UNLIKELY(!pNoteEditorWidget)) {
            continue;
        }

        if (pNoteEditorWidget->noteLocalUid() != noteLocalUid) {
            continue;
        }

        if (Q_UNLIKELY(!pNoteEditorWidget->isModified())) {
            QNINFO(QStringLiteral("The note editor widget doesn't appear to contain a note that needs saving"));
            return;
        }

        ErrorString errorDescription;
        NoteEditorWidget::NoteSaveStatus::type res = pNoteEditorWidget->checkAndSaveModifiedNote(errorDescription);
        if (Q_UNLIKELY(res != NoteEditorWidget::NoteSaveStatus::Ok))
        {
            ErrorString error(QT_TRANSLATE_NOOP("", "Couldn't save the note"));
            error.additionalBases().append(errorDescription.base());
            error.additionalBases().append(errorDescription.additionalBases());
            error.details() = errorDescription.details();
            QNWARNING(error << QStringLiteral(", note local uid = ") << noteLocalUid);
            emit notifyError(error);
        }

        return;
    }

    // If we got here, no target note editor widget was found
    ErrorString error(QT_TRANSLATE_NOOP("", "Internal error: can't save the note within the chosen note editor, "
                                        "can't find the editor to be closed by note local uid"));
    QNWARNING(error << QStringLiteral(", note local uid = ") << noteLocalUid);
    emit notifyError(error);
    return;
}

void NoteEditorTabWidgetManager::insertNoteEditorWidget(NoteEditorWidget * pNoteEditorWidget)
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::insertNoteEditorWidget: ") << pNoteEditorWidget->noteLocalUid());

    QObject::connect(pNoteEditorWidget, QNSIGNAL(NoteEditorWidget,titleOrPreviewChanged,QString),
                     this, QNSLOT(NoteEditorTabWidgetManager,onNoteTitleOrPreviewTextChanged,QString));
    QObject::connect(pNoteEditorWidget, QNSIGNAL(NoteEditorWidget,resolved),
                     this, QNSLOT(NoteEditorTabWidgetManager,onNoteEditorWidgetResolved));
    QObject::connect(pNoteEditorWidget, QNSIGNAL(NoteEditorWidget,invalidated),
                     this, QNSLOT(NoteEditorTabWidgetManager,onNoteEditorWidgetInvalidated));
    QObject::connect(pNoteEditorWidget, QNSIGNAL(NoteEditorWidget,noteLoaded),
                     this, QNSLOT(NoteEditorTabWidgetManager,onNoteLoadedInEditor));
    QObject::connect(pNoteEditorWidget, QNSIGNAL(NoteEditorWidget,notifyError,ErrorString),
                     this, QNSLOT(NoteEditorTabWidgetManager,onNoteEditorError,ErrorString));

    QString tabName = shortenTabName(pNoteEditorWidget->titleOrPreview());

    int tabIndex = m_pTabWidget->indexOf(pNoteEditorWidget);
    if (tabIndex < 0) {
        tabIndex = m_pTabWidget->addTab(pNoteEditorWidget, tabName);
    }
    else {
        m_pTabWidget->setTabText(tabIndex, tabName);
    }

    m_pTabWidget->setCurrentIndex(tabIndex);

    m_localUidsOfNotesInTabbedEditors.push_back(pNoteEditorWidget->noteLocalUid());
    QNTRACE(QStringLiteral("Added tabbed note local uid: ") << pNoteEditorWidget->noteLocalUid()
            << QStringLiteral(", the number of tabbed note local uids = ") << m_localUidsOfNotesInTabbedEditors.size());
    persistLocalUidsOfOpenNotes();

    int currentNumNotesInTabs = numNotesInTabs();

    if (currentNumNotesInTabs > 1) {
        m_pTabWidget->tabBar()->show();
        m_pTabWidget->setTabsClosable(true);
    }
    else {
        m_pTabWidget->tabBar()->hide();
        m_pTabWidget->setTabsClosable(false);
    }

    if (currentNumNotesInTabs <= m_maxNumNotesInTabs) {
        QNDEBUG(QStringLiteral("The addition of note ") << pNoteEditorWidget->noteLocalUid()
                << QStringLiteral(" doesn't cause the overflow of max allowed number of note editor tabs"));
        return;
    }

    checkAndCloseOlderNoteEditorTabs();
}

void NoteEditorTabWidgetManager::checkAndCloseOlderNoteEditorTabs()
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
        auto it = std::find(m_localUidsOfNotesInTabbedEditors.begin(), m_localUidsOfNotesInTabbedEditors.end(), noteLocalUid);
        if (it == m_localUidsOfNotesInTabbedEditors.end()) {
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

void NoteEditorTabWidgetManager::setCurrentNoteEditorWidget(const QString & noteLocalUid)
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::setCurrentNoteEditorWidget: ") << noteLocalUid);

    for(int i = 0; i < m_pTabWidget->count(); ++i)
    {
        NoteEditorWidget * pNoteEditorWidget = qobject_cast<NoteEditorWidget*>(m_pTabWidget->widget(i));
        if (Q_UNLIKELY(!pNoteEditorWidget)) {
            continue;
        }

        if (Q_UNLIKELY(pNoteEditorWidget == m_pBlankNoteEditor)) {
            continue;
        }

        if (pNoteEditorWidget->noteLocalUid() == noteLocalUid) {
            m_pTabWidget->setCurrentIndex(i);
            break;
        }
    }
}

void NoteEditorTabWidgetManager::connectToLocalStorage()
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::connectToLocalStorage"));

    QObject::connect(this, QNSIGNAL(NoteEditorTabWidgetManager,requestAddNote,Note,QUuid),
                     &m_localStorageWorker, QNSLOT(LocalStorageManagerThreadWorker,onAddNoteRequest,Note,QUuid));
    QObject::connect(&m_localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,addNoteComplete,Note,QUuid),
                     this, QNSLOT(NoteEditorTabWidgetManager,onAddNoteComplete,Note,QUuid));
    QObject::connect(&m_localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,addNoteFailed,Note,ErrorString,QUuid),
                     this, QNSLOT(NoteEditorTabWidgetManager,onAddNoteFailed,Note,ErrorString,QUuid));
}

void NoteEditorTabWidgetManager::disconnectFromLocalStorage()
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::disconnectFromLocalStorage"));

    QObject::disconnect(this, QNSIGNAL(NoteEditorTabWidgetManager,requestAddNote,Note,QUuid),
                        &m_localStorageWorker, QNSLOT(LocalStorageManagerThreadWorker,onAddNoteRequest,Note,QUuid));
    QObject::disconnect(&m_localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,addNoteComplete,Note,QUuid),
                        this, QNSLOT(NoteEditorTabWidgetManager,onAddNoteComplete,Note,QUuid));
    QObject::disconnect(&m_localStorageWorker, QNSIGNAL(LocalStorageManagerThreadWorker,addNoteFailed,Note,ErrorString,QUuid),
                        this, QNSLOT(NoteEditorTabWidgetManager,onAddNoteFailed,Note,ErrorString,QUuid));
}

void NoteEditorTabWidgetManager::setupFileIO()
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::setupFileIO"));

    if (!m_pIOThread) {
        m_pIOThread = new QThread;
        QObject::connect(m_pIOThread, QNSIGNAL(QThread,finished), m_pIOThread, QNSLOT(QThread,deleteLater));
        m_pIOThread->start(QThread::LowPriority);
    }

    if (!m_pFileIOThreadWorker) {
        m_pFileIOThreadWorker = new FileIOThreadWorker;
        m_pFileIOThreadWorker->moveToThread(m_pIOThread);
    }
}

void NoteEditorTabWidgetManager::setupSpellChecker()
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::setupSpellChecker"));

    if (!m_pSpellChecker) {
        m_pSpellChecker = new SpellChecker(m_pFileIOThreadWorker, this);
    }
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

void NoteEditorTabWidgetManager::persistLocalUidsOfOpenNotes()
{
    QNDEBUG("NoteEditorTabWidgetManager::persistLocalUidsOfOpenNotes");

    QStringList openNotesLocalUids;
    size_t size = m_localUidsOfNotesInTabbedEditors.size();
    openNotesLocalUids.reserve(static_cast<int>(size));

    for(auto it = m_localUidsOfNotesInTabbedEditors.begin(),
        end = m_localUidsOfNotesInTabbedEditors.end(); it != end; ++it)
    {
        openNotesLocalUids << *it;
    }

    ApplicationSettings appSettings;
    appSettings.beginGroup(QStringLiteral("NoteEditor"));
    appSettings.setValue(OPEN_NOTES_LOCAL_UIDS_SETTINGS_KEY, openNotesLocalUids);
    appSettings.endGroup();
}

void NoteEditorTabWidgetManager::restoreLastOpenNotes()
{
    QNDEBUG(QStringLiteral("NoteEditorTabWidgetManager::restoreLastOpenNotes"));

    ApplicationSettings appSettings;
    appSettings.beginGroup(QStringLiteral("NoteEditor"));
    QStringList lastOpenNoteLocalUids = appSettings.value(OPEN_NOTES_LOCAL_UIDS_SETTINGS_KEY).toStringList();
    appSettings.endGroup();

    if (lastOpenNoteLocalUids.isEmpty()) {
        QNDEBUG(QStringLiteral("No last open note local uids"));
        return;
    }

    for(auto it = lastOpenNoteLocalUids.constBegin(), end = lastOpenNoteLocalUids.constEnd(); it != end; ++it) {
        addNote(*it);
    }

    return;
}

} // namespace quentier

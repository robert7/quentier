#include "NoteEditor_p.h"
#include "NoteEditorPage.h"
#include "EncryptedAreaPlugin.h"

#ifndef USE_QT_WEB_ENGINE
#include <qute_note/note_editor/NoteEditorPluginFactory.h>
#include <QWebFrame>
typedef QWebSettings WebSettings;
#else
#include "JavaScriptInOrderExecutor.h"
#include "NoteDecryptionDialog.h"
#include "WebSocketClientWrapper.h"
#include "WebSocketTransport.h"
#include <QtWebSockets/QWebSocketServer>
#include <QtWebChannel>
#include <QWebEngineSettings>
typedef QWebEngineSettings WebSettings;
#endif

#include <qute_note/note_editor/ResourceFileStorageManager.h>
#include <qute_note/exception/ResourceLocalFileStorageFolderNotFoundException.h>
#include <qute_note/exception/NoteEditorPluginInitializationException.h>
#include <qute_note/types/Note.h>
#include <qute_note/types/Notebook.h>
#include <qute_note/types/ResourceWrapper.h>
#include <qute_note/enml/ENMLConverter.h>
#include <qute_note/utility/Utility.h>
#include <qute_note/types/ResourceAdapter.h>
#include <qute_note/logging/QuteNoteLogger.h>
#include <qute_note/utility/FileIOThreadWorker.h>
#include <qute_note/utility/QuteNoteCheckPtr.h>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QByteArray>
#include <QImage>
#include <QDropEvent>
#include <QMimeType>
#include <QMimeData>
#include <QMimeDatabase>
#include <QThread>
#include <QXmlStreamReader>

namespace qute_note {

NoteEditorPrivate::NoteEditorPrivate(NoteEditor & noteEditor) :
    QObject(&noteEditor),
    m_noteEditorPageFolderPath(),
    m_jQuery(),
    m_resizableColumnsPlugin(),
    m_onFixedWidthTableResize(),
    m_getSelectionHtml(),
    m_replaceSelectionWithHtml(),
    m_provideSrcForResourceImgTags(),
    m_setupEnToDoTags(),
#ifdef USE_QT_WEB_ENGINE
    m_provideSrcForGenericResourceIcons(),
    m_provideSrcAndOnClickScriptForEnCryptImgTags(),
    m_pWebSocketServer(new QWebSocketServer("QWebChannel server", QWebSocketServer::NonSecureMode, this)),
    m_pWebSocketClientWrapper(new WebSocketClientWrapper(m_pWebSocketServer, this)),
    m_pWebChannel(new QWebChannel(this)),
    m_pPageMutationHandler(new PageMutationHandler(this)),
    m_pMimeTypeIconJavaScriptHandler(nullptr),
    m_pEnCryptElementClickHandler(new EnCryptElementClickHandler(this)),
    m_pJavaScriptInOrderExecutor(new JavaScriptInOrderExecutor(noteEditor, this)),
    m_webSocketServerPort(0),
#endif
    m_writeNoteHtmlToFileRequestId(),
    m_isPageEditable(false),
    m_pendingConversionToNote(false),
    m_pNote(nullptr),
    m_pNotebook(nullptr),
    m_modified(false),
    m_watchingForContentChange(false),
    m_contentChangedSinceWatchingStart(false),
    m_secondsToWaitBeforeConversionStart(30),
    m_pageToNoteContentPostponeTimerId(0),
    m_encryptionManager(new EncryptionManager),
    m_decryptedTextManager(),
    m_enmlConverter(),
#ifndef USE_QT_WEB_ENGINE
    m_pluginFactory(nullptr),
#endif
    m_pagePrefix("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"http://www.w3.org/TR/html4/strict.dtd\">"
                 "<html><head>"
                 "<meta http-equiv=\"Content-Type\" content=\"text/html\" charset=\"UTF-8\" />"
                 "<link rel=\"stylesheet\" type=\"text/css\" href=\"qrc:/css/en-crypt.css\">"
                 "<link rel=\"stylesheet\" type=\"text/css\" href=\"qrc:/css/hover.css\">"
                 "<link rel=\"stylesheet\" type=\"text/css\" href=\"qrc:/css/en-decrypted.css\">"
                 "<link rel=\"stylesheet\" type=\"text/css\" href=\"qrc:/css/en-media-generic.css\">"
                 "<link rel=\"stylesheet\" type=\"text/css\" href=\"qrc:/css/en-todo.css\">"
                 "<title></title></head>"),
    m_enmlCachedMemory(),
    m_htmlCachedMemory(),
    m_errorCachedMemory(),
    m_pIOThread(nullptr),
    m_pResourceFileStorageManager(nullptr),
    m_pFileIOThreadWorker(nullptr),
    m_resourceLocalFileInfoCache(),
    m_pResourceLocalFileInfoJavaScriptHandler(new ResourceLocalFileInfoJavaScriptHandler(m_resourceLocalFileInfoCache, this)),
    m_resourceLocalFileStorageFolder(),
    m_resourceLocalGuidBySaveToStorageRequestIds(),
    m_droppedFileNamesAndMimeTypesByReadRequestIds(),
    q_ptr(&noteEditor)
{
    QString initialHtml = m_pagePrefix + "<body></body></html>";
    m_noteEditorPageFolderPath = applicationPersistentStoragePath() + "/NoteEditorPage";

    setupFileIO();

#ifdef USE_QT_WEB_ENGINE
    setupWebSocketServer();
    setupJavaScriptObjects();
#endif

    setupNoteEditorPage();
    setupScripts();

    Q_Q(NoteEditor);
    m_resourceLocalFileStorageFolder = ResourceFileStorageManager::resourceFileStorageLocation(q);
    if (m_resourceLocalFileStorageFolder.isEmpty()) {
        QString error = QT_TR_NOOP("Can't get resource file storage folder");
        QNWARNING(error);
        throw ResourceLocalFileStorageFolderNotFoundException(error);
    }
    QNTRACE("Resource local file storage folder: " << m_resourceLocalFileStorageFolder);

    m_writeNoteHtmlToFileRequestId = QUuid::createUuid();
    emit writeNoteHtmlToFile(m_noteEditorPageFolderPath + "/index.html", initialHtml.toLocal8Bit(),
                             m_writeNoteHtmlToFileRequestId);
    QNTRACE("Emitted the request to write the index html file, request id: " << m_writeNoteHtmlToFileRequestId);
}

NoteEditorPrivate::~NoteEditorPrivate()
{
    delete m_pNote;
    delete m_pNotebook;

    m_pIOThread->quit();
}

void NoteEditorPrivate::onNoteLoadFinished(bool ok)
{
    QNDEBUG("NoteEditorPrivate::onNoteLoadFinished: ok = " << (ok ? "true" : "false"));

    if (!ok) {
        QNWARNING("Note page was not loaded successfully");
        return;
    }

    Q_Q(NoteEditor);

#ifndef USE_QT_WEB_ENGINE
    QWebPage * page = q->page();
    if (Q_UNLIKELY(!page)) {
        return;
    }

    QWebFrame * frame = page->mainFrame();
    if (Q_UNLIKELY(!frame)) {
        return;
    }

    frame->addToJavaScriptWindowObject("resourceCache", m_pResourceLocalFileInfoJavaScriptHandler,
                                       QScriptEngine::QtOwnership);

    Q_UNUSED(frame->evaluateJavaScript(m_jQuery));
    Q_UNUSED(frame->evaluateJavaScript(m_resizableColumnsPlugin));
    Q_UNUSED(frame->evaluateJavaScript(m_onFixedWidthTableResize));
    Q_UNUSED(frame->evaluateJavaScript(m_getSelectionHtml));
    Q_UNUSED(frame->evaluateJavaScript(m_replaceSelectionWithHtml));
    Q_UNUSED(frame->evaluateJavaScript(m_provideSrcForResourceImgTags));
    Q_UNUSED(frame->evaluateJavaScript(m_setupEnToDoTags));
#else
    QWebEnginePage * page = q->page();
    if (!page) {
        return;
    }

    m_pJavaScriptInOrderExecutor->clear();
    m_pJavaScriptInOrderExecutor->append(m_pageMutationObserverJs);
    m_pJavaScriptInOrderExecutor->append(m_qWebChannelJs);

    // Expose websocket server port number to JavaScript
    m_pJavaScriptInOrderExecutor->append(QString("(function(){window.websocketserverport = ") +
                                         QString::number(m_webSocketServerPort) + QString("})();"));

    m_pJavaScriptInOrderExecutor->append(m_qWebChannelSetupJs);
    m_pJavaScriptInOrderExecutor->append(m_jQuery);
    m_pJavaScriptInOrderExecutor->append(m_resizableColumnsPlugin);
    m_pJavaScriptInOrderExecutor->append(m_onFixedWidthTableResize);
    m_pJavaScriptInOrderExecutor->append(m_getSelectionHtml);
    m_pJavaScriptInOrderExecutor->append(m_replaceSelectionWithHtml);
    m_pJavaScriptInOrderExecutor->append(m_provideSrcForResourceImgTags);
    m_pJavaScriptInOrderExecutor->append(m_setupEnToDoTags);
    m_pJavaScriptInOrderExecutor->append(m_provideSrcAndOnClickScriptForEnCryptImgTags);
    m_pJavaScriptInOrderExecutor->append(m_provideSrcForGenericResourceIcons);

    setPageEditable(true);
#endif

    updateColResizableTableBindings();
    saveNoteResourcesToLocalFiles();

#ifdef USE_QT_WEB_ENGINE
    provideSrcAndOnClickScriptForImgEnCryptTags();
#endif

    QNTRACE("Evaluated all JavaScript helper functions");

#ifndef USE_QT_WEB_ENGINE
    m_htmlCachedMemory = q->page()->mainFrame()->toHtml();
    onPageHtmlReceived(m_htmlCachedMemory);
#else
    q->page()->toHtml(HtmlRetrieveFunctor<QString>(this, &NoteEditorPrivate::onPageHtmlReceived));
#endif
}

void NoteEditorPrivate::onContentChanged()
{
    QNTRACE("NoteEditorPrivate::onContentChanged");
    m_modified = true;

    if (Q_LIKELY(m_watchingForContentChange)) {
        m_contentChangedSinceWatchingStart = true;
        return;
    }

    m_pageToNoteContentPostponeTimerId = startTimer(SEC_TO_MSEC(m_secondsToWaitBeforeConversionStart));
    m_watchingForContentChange = true;
    m_contentChangedSinceWatchingStart = false;
    QNTRACE("Started timer to postpone note editor page's content to ENML conversion");
}

void NoteEditorPrivate::onResourceSavedToStorage(QUuid requestId, QByteArray dataHash,
                                                 int errorCode, QString errorDescription)
{
    QNTRACE("NoteEditorPrivate::onResourceSavedToStorage: requestId = " << requestId
            << ", data hash = " << dataHash << ", error code = " << errorCode
            << ", error description: " << errorDescription);

    auto it = m_resourceLocalGuidBySaveToStorageRequestIds.find(requestId);
    if (it == m_resourceLocalGuidBySaveToStorageRequestIds.end()) {
        return;
    }

    if (errorCode != 0) {
        errorDescription.prepend(QT_TR_NOOP("Can't write resource to local file: "));
        emit notifyError(errorDescription);
        QNWARNING(errorDescription + ", error code = " << errorCode);
        return;
    }

    const QString & localGuid = it.value();

    if (m_pNote)
    {
        QList<ResourceAdapter> resourceAdapters = m_pNote->resourceAdapters();
        const int numResources = resourceAdapters.size();
        for(int i = 0; i < numResources; ++i)
        {
            ResourceAdapter & resourceAdapter = resourceAdapters[i];
            if (resourceAdapter.localGuid() != localGuid) {
                continue;
            }

            if (!resourceAdapter.hasDataHash()) {
                resourceAdapter.setDataHash(dataHash);
            }
        }
    }

    QString dataHashStr = QString::fromLocal8Bit(dataHash.constData(), dataHash.size());

    QString resourceLocalFilePath = m_resourceLocalFileStorageFolder + "/" + localGuid;

    m_resourceLocalFileInfoCache[dataHashStr] = resourceLocalFilePath;
    QNTRACE("Cached resource local file path " << resourceLocalFilePath
            << " for resource hash " << dataHashStr);

    Q_UNUSED(m_resourceLocalGuidBySaveToStorageRequestIds.erase(it));

    if (m_resourceLocalGuidBySaveToStorageRequestIds.isEmpty()) {
        QNTRACE("All current note's resources were saved to local file storage and are actual. "
                "Will set filepaths to these local files to src attributes of img resource tags");
        provideScrForImgResourcesFromCache();
    }
}

void NoteEditorPrivate::onDroppedFileRead(bool success, QString errorDescription,
                                          QByteArray data, QUuid requestId)
{
    QNTRACE("NoteEditorPrivate::onDroppedFileRead: success = " << (success ? "true" : "false")
            << ", error description = " << errorDescription << ", request id = " << requestId);

    auto it = m_droppedFileNamesAndMimeTypesByReadRequestIds.find(requestId);
    if (it == m_droppedFileNamesAndMimeTypesByReadRequestIds.end()) {
        return;
    }

    const QString fileName = it.value().first;
    const QMimeType mimeType = it.value().second;

    Q_UNUSED(m_droppedFileNamesAndMimeTypesByReadRequestIds.erase(it));

    if (!success) {
        QNDEBUG("Could not read the content of the dropped file for request id " << requestId
                << ": " << errorDescription);
        return;
    }

    if (!m_pNote) {
        QNDEBUG("Current note is empty");
        return;
    }

    QNDEBUG("Successfully read the content of the dropped file for request id " << requestId);
    QByteArray dataHash;
    QString newResourceLocalGuid = attachResourceToNote(data, dataHash, mimeType, fileName);

    QUuid saveResourceToStorageRequestId = QUuid::createUuid();
    m_resourceLocalGuidBySaveToStorageRequestIds[saveResourceToStorageRequestId] = newResourceLocalGuid;
    emit saveResourceToStorage(newResourceLocalGuid, data, dataHash, saveResourceToStorageRequestId);
}

#ifdef USE_QT_WEB_ENGINE
void NoteEditorPrivate::onEnCryptElementClicked(QString encryptedText, QString cipher, QString length, QString hint)
{
    QNDEBUG("NoteEditorPrivate::onEnCryptElementClicked");

    if (cipher.isEmpty()) {
        cipher = "AES";
    }

    if (length.isEmpty()) {
        length = "128";
    }

    bool conversionResult = false;
    size_t keyLength = static_cast<size_t>(length.toInt(&conversionResult));
    if (!conversionResult) {
        QNFATAL("NoteEditorPrivate::onEnCryptElementClicked: can't convert encryption key from string to number: " << length);
        return;
    }

    Q_Q(NoteEditor);
    QScopedPointer<NoteDecryptionDialog> pDecryptionDialog(new NoteDecryptionDialog(encryptedText,
                                                                                    cipher, hint, keyLength,
                                                                                    m_encryptionManager,
                                                                                    m_decryptedTextManager, q));
    pDecryptionDialog->setWindowModality(Qt::WindowModal);
    QObject::connect(pDecryptionDialog.data(), QNSIGNAL(NoteDecryptionDialog,accepted,QString,QString,bool),
                     this,QNSLOT(NoteEditorPrivate,onEncryptedAreaDecryption,QString,QString,bool));
    QNTRACE("Will exec note decryption dialog now");
    pDecryptionDialog->exec();
    QNTRACE("Executed note decryption dialog");
}

void NoteEditorPrivate::onJavaScriptLoaded()
{
    QNDEBUG("NoteEditorPrivate::onJavaScriptLoaded");
}

#endif

void NoteEditorPrivate::onWriteFileRequestProcessed(bool success, QString errorDescription, QUuid requestId)
{
    if (requestId == m_writeNoteHtmlToFileRequestId)
    {
        QNDEBUG("Write note html to file completed: success = " << (success ? "true" : "false")
                << ", error description = " << errorDescription << ", request id = " << requestId);

        m_writeNoteHtmlToFileRequestId = QUuid();

        if (!success) {
            clearEditorContent();
            errorDescription.prepend(QT_TR_NOOP("Could not write note html to file: "));
            emit notifyError(errorDescription);
            QNWARNING(errorDescription);
            return;
        }

        Q_Q(NoteEditor);
        QUrl url("file://" + m_noteEditorPageFolderPath + "/index.html");
        q->load(url);
        QNTRACE("Loaded url: " << url);
    }
}

void NoteEditorPrivate::timerEvent(QTimerEvent * event)
{
    QNDEBUG("NoteEditorPrivate::timerEvent: " << (event ? QString::number(event->timerId()) : "<null>"));

    if (!event) {
        return;
    }

    if (event->timerId() == m_pageToNoteContentPostponeTimerId)
    {
        if (m_contentChangedSinceWatchingStart)
        {
            QNTRACE("Note editor page's content has been changed lately, "
                    "the editing is most likely in progress now, postponing "
                    "the conversion to ENML");
            m_contentChangedSinceWatchingStart = false;
            return;
        }

        QNTRACE("Looks like the note editing has stopped for a while, "
                "will convert the note editor page's content to ENML");
        bool res = htmlToNoteContent(m_errorCachedMemory);
        if (!res) {
            emit notifyError(m_errorCachedMemory);
        }

        killTimer(m_pageToNoteContentPostponeTimerId);
        m_pageToNoteContentPostponeTimerId = 0;

        m_watchingForContentChange = false;
        m_contentChangedSinceWatchingStart = false;
    }
}

void NoteEditorPrivate::clearEditorContent()
{
    QNDEBUG("NoteEditorPrivate::clearEditorContent");

    if (m_pageToNoteContentPostponeTimerId != 0) {
        killTimer(m_pageToNoteContentPostponeTimerId);
        m_pageToNoteContentPostponeTimerId = 0;
    }

    m_watchingForContentChange = false;
    m_contentChangedSinceWatchingStart = false;

    m_modified = false;

    QString initialHtml = m_pagePrefix + "<body></body></html>";
    m_writeNoteHtmlToFileRequestId = QUuid::createUuid();
    emit writeNoteHtmlToFile(m_noteEditorPageFolderPath + "/index.html", initialHtml.toLocal8Bit(),
                             m_writeNoteHtmlToFileRequestId);
}

void NoteEditorPrivate::noteToEditorContent()
{
    QNDEBUG("NoteEditorPrivate::noteToEditorContent");

    if (!m_pNote) {
        QNDEBUG("No note has been set yet");
        clearEditorContent();
        return;
    }

    if (!m_pNote->hasContent()) {
        QNDEBUG("Note without content was inserted into NoteEditor");
        clearEditorContent();
        return;
    }

    m_htmlCachedMemory.resize(0);
    bool res = m_enmlConverter.noteContentToHtml(m_pNote->content(), m_htmlCachedMemory, m_errorCachedMemory,
                                                 m_decryptedTextManager
#ifndef USE_QT_WEB_ENGINE
                                                 , m_pluginFactory
#endif
                                                 );
    if (!res) {
        QNWARNING("Can't convert note's content to HTML: " << m_errorCachedMemory);
        emit notifyError(m_errorCachedMemory);
        clearEditorContent();
        return;
    }

    int bodyTagIndex = m_htmlCachedMemory.indexOf("<body>");
    if (bodyTagIndex < 0) {
        m_errorCachedMemory = QT_TR_NOOP("can't find <body> tag in the result of note to HTML conversion");
        QNWARNING(m_errorCachedMemory << ", note content: " << m_pNote->content()
                  << ", html: " << m_htmlCachedMemory);
        emit notifyError(m_errorCachedMemory);
        clearEditorContent();
        return;
    }

    m_htmlCachedMemory.replace(0, bodyTagIndex, m_pagePrefix);

    int bodyClosingTagIndex = m_htmlCachedMemory.indexOf("</body>");
    if (bodyClosingTagIndex < 0) {
        m_errorCachedMemory = QT_TR_NOOP("Can't find </body> tag in the result of note to HTML conversion");
        QNWARNING(m_errorCachedMemory << ", note content: " << m_pNote->content()
                  << ", html: " << m_htmlCachedMemory);
        emit notifyError(m_errorCachedMemory);
        clearEditorContent();
        return;
    }

    m_htmlCachedMemory.insert(bodyClosingTagIndex + 7, "</html>");

    m_htmlCachedMemory.replace("<br></br>", "</br>");   // Webkit-specific fix

    bool readOnly = false;
    if (m_pNote->hasActive() && !m_pNote->active()) {
        QNDEBUG("Current note is not active, setting it to read-only state");
        setPageEditable(false);
        readOnly = true;
    }
    else if (m_pNotebook->hasRestrictions())
    {
        const qevercloud::NotebookRestrictions & restrictions = m_pNotebook->restrictions();
        if (restrictions.noUpdateNotes.isSet() && restrictions.noUpdateNotes.ref()) {
            QNDEBUG("Notebook restrictions forbid the note modification, setting note's content to read-only state");
#ifndef USE_QT_WEB_ENGINE
            Q_Q(NoteEditor);
            QWebPage * page = q->page();
            if (Q_LIKELY(page)) {
                page->setContentEditable(false);
            }
#else
            setPageEditable(false);
#endif
            readOnly = true;
        }
    }

    if (!readOnly) {
        QNDEBUG("Nothing prevents user to modify the note, allowing it in the editor");
        setPageEditable(true);
    }

    m_writeNoteHtmlToFileRequestId = QUuid::createUuid();
    emit writeNoteHtmlToFile(m_noteEditorPageFolderPath + "/index.html", m_htmlCachedMemory.toLocal8Bit(),
                             m_writeNoteHtmlToFileRequestId);
    QNTRACE("Done setting the current note and notebook");
}

void NoteEditorPrivate::updateColResizableTableBindings()
{
    QNDEBUG("NoteEditorPrivate::updateColResizableTableBindings");

    bool readOnly = !isPageEditable();

    QString colResizable = "$(\"table\").colResizable({";
    if (readOnly) {
        colResizable += "disable:true});";
    }
    else {
        colResizable += "liveDrag:true, "
                        "gripInnerHtml:\"<div class=\\'grip\\'></div>\", "
                        "draggingClass:\"dragging\", "
                        "postbackSafe:true, "
                        "onResize:onFixedWidthTableResized"
                        "});";
    }

    QNTRACE("colResizable js code: " << colResizable);

#ifndef USE_QT_WEB_ENGINE
    Q_Q(NoteEditor);
    q->page()->mainFrame()->evaluateJavaScript(colResizable);
#else
    m_pJavaScriptInOrderExecutor->append(colResizable);
    if (!m_pJavaScriptInOrderExecutor->inProgress()) {
        m_pJavaScriptInOrderExecutor->start();
    }
#endif
}

bool NoteEditorPrivate::htmlToNoteContent(QString & errorDescription)
{
    QNDEBUG("NoteEditorPrivate::htmlToNoteContent");

    if (!m_pNote) {
        errorDescription = QT_TR_NOOP("No note was set to note editor");
        emit cantConvertToNote(errorDescription);
        return false;
    }

    if (m_pNote->hasActive() && !m_pNote->active()) {
        errorDescription = QT_TR_NOOP("Current note is marked as read-only, the changes won't be saved");
        QNINFO(errorDescription << ", note: local guid = " << m_pNote->localGuid()
               << ", guid = " << (m_pNote->hasGuid() ? m_pNote->guid() : "<null>")
               << ", title = " << (m_pNote->hasTitle() ? m_pNote->title() : "<null>"));
        emit cantConvertToNote(errorDescription);
        return false;
    }

    if (m_pNotebook && m_pNotebook->hasRestrictions())
    {
        const qevercloud::NotebookRestrictions & restrictions = m_pNotebook->restrictions();
        if (restrictions.noUpdateNotes.isSet() && restrictions.noUpdateNotes.ref()) {
            errorDescription = QT_TR_NOOP("The notebook the current note belongs to doesn't allow notes modification, "
                                          "the changes won't be saved");
            QNINFO(errorDescription << ", note: local guid = " << m_pNote->localGuid()
                   << ", guid = " << (m_pNote->hasGuid() ? m_pNote->guid() : "<null>")
                   << ", title = " << (m_pNote->hasTitle() ? m_pNote->title() : "<null>")
                   << ", notebook: local guid = " << m_pNotebook->localGuid() << ", guid = "
                   << (m_pNotebook->hasGuid() ? m_pNotebook->guid() : "<null>") << ", name = "
                   << (m_pNotebook->hasName() ? m_pNotebook->name() : "<null>"));
            emit cantConvertToNote(errorDescription);
            return false;
        }
    }

    Q_Q(const NoteEditor);

    m_pendingConversionToNote = true;

#ifndef USE_QT_WEB_ENGINE
    m_htmlCachedMemory = q->page()->mainFrame()->toHtml();
    onPageHtmlReceived(m_htmlCachedMemory);
#else
    q->page()->toHtml(HtmlRetrieveFunctor<QString>(this, &NoteEditorPrivate::onPageHtmlReceived));
#endif

    return true;
}

void NoteEditorPrivate::saveNoteResourcesToLocalFiles()
{
    QNDEBUG("NoteEditorPrivate::saveNoteResourcesToLocalFiles");

    if (!m_pNote) {
        QNTRACE("No note is set for the editor");
        return;
    }

    QList<ResourceAdapter> resourceAdapters = m_pNote->resourceAdapters();
    if (resourceAdapters.isEmpty()) {
        QNTRACE("Note has no resources, nothing to do");
        return;
    }

    auto resourceAdaptersConstBegin = resourceAdapters.constBegin();
    auto resourceAdaptersConstEnd = resourceAdapters.constEnd();

    size_t numPendingResourceWritesToLocalFiles = 0;

    for(auto it = resourceAdaptersConstBegin; it != resourceAdaptersConstEnd; ++it)
    {
        const ResourceAdapter & resourceAdapter = *it;

        if (!resourceAdapter.hasDataBody() && !resourceAdapter.hasAlternateDataBody()) {
            QNINFO("Detected resource without data body: " << resourceAdapter);
            continue;
        }

        if (!resourceAdapter.hasDataHash() && !resourceAdapter.hasAlternateDataHash()) {
            QNINFO("Detected resource without data hash: " << resourceAdapter);
            continue;
        }

        const QByteArray & dataBody = (resourceAdapter.hasDataBody()
                                       ? resourceAdapter.dataBody()
                                       : resourceAdapter.alternateDataBody());

        const QByteArray & dataHash = (resourceAdapter.hasDataHash()
                                       ? resourceAdapter.dataHash()
                                       : resourceAdapter.alternateDataHash());

        QString dataHashStr = QString::fromLocal8Bit(dataHash.constData(), dataHash.size());

        QNTRACE("Found current note's resource corresponding to the data hash "
                << dataHashStr << ": " << resourceAdapter);

        if (!m_resourceLocalFileInfoCache.contains(dataHashStr))
        {
            const QString resourceLocalGuid = resourceAdapter.localGuid();
            QUuid saveResourceRequestId = QUuid::createUuid();
            m_resourceLocalGuidBySaveToStorageRequestIds[saveResourceRequestId] = resourceLocalGuid;
            emit saveResourceToStorage(resourceLocalGuid, dataBody, dataHash, saveResourceRequestId);
            QNTRACE("Sent request to save resource to file storage: request id = " << saveResourceRequestId
                    << ", resource local guid = " << resourceLocalGuid << ", data hash = " << dataHash);
            ++numPendingResourceWritesToLocalFiles;
        }
    }

    if (numPendingResourceWritesToLocalFiles == 0) {
        QNTRACE("All current note's resources are written to local files and are actual. "
                "Will set filepaths to these local files to src attributes of img resource tags");
        provideScrForImgResourcesFromCache();
        return;
    }

    QNTRACE("Scheduled writing of " << numPendingResourceWritesToLocalFiles
            << " to local files, will wait until they are written "
            "and add the src attributes to img resources when the files are ready");
}

void NoteEditorPrivate::provideScrForImgResourcesFromCache()
{
    QNDEBUG("NoteEditorPrivate::provideScrForImgResourcesFromCache");

#ifndef USE_QT_WEB_ENGINE
    Q_Q(NoteEditor);
    q->page()->mainFrame()->evaluateJavaScript("provideSrcForResourceImgTags();");
#else
    m_pJavaScriptInOrderExecutor->append("provideSrcForResourceImgTags();");
    if (!m_pJavaScriptInOrderExecutor->inProgress()) {
        m_pJavaScriptInOrderExecutor->start();
    }
#endif
}

#ifdef USE_QT_WEB_ENGINE
void NoteEditorPrivate::provideSrcAndOnClickScriptForImgEnCryptTags()
{
    QNDEBUG("NoteEditorPrivate::provideSrcAndOnClickScriptForImgEnCryptTags");

    if (!m_pNote) {
        QNTRACE("No note is set for the editor");
        return;
    }

    if (!m_pNote->containsEncryption()) {
        QNTRACE("Current note doesn't contain any encryption, nothing to do");
        return;
    }

    QString iconPath = "qrc:/encrypted_area_icons/en-crypt/en-crypt.png";

    QString javascript = "provideSrcAndOnClickScriptForEnCryptImgTags(\"" + iconPath + "\")";
    m_pJavaScriptInOrderExecutor->append(javascript);
    if (!m_pJavaScriptInOrderExecutor->inProgress()) {
        m_pJavaScriptInOrderExecutor->start();
    }
    QNDEBUG("Queued javascript command to provide src for img tags: " << javascript);
}

void NoteEditorPrivate::setupWebSocketServer()
{
    QNDEBUG("NoteEditorPrivate::setupWebSocketServer");

    if (!m_pWebSocketServer->listen(QHostAddress::LocalHost, 0)) {
        QNFATAL("Cannot open web socket server: " << m_pWebSocketServer->errorString());
        // TODO: throw appropriate exception
        return;
    }

    m_webSocketServerPort = m_pWebSocketServer->serverPort();
    QNDEBUG("Using automatically selected websocket server port " << m_webSocketServerPort);

    QObject::connect(m_pWebSocketClientWrapper, &WebSocketClientWrapper::clientConnected,
                     m_pWebChannel, &QWebChannel::connectTo);
}

void NoteEditorPrivate::setupJavaScriptObjects()
{
    QNDEBUG("NoteEditorPrivate::setupJavaScriptObjects");

    Q_Q(NoteEditor);

    // As long as QWebEnginePage doesn't offer a convenience signal on its content change,
    // need to reinvent the wheel using JavaScript
    QObject::connect(m_pPageMutationHandler, &PageMutationHandler::contentsChanged,
                     q, &NoteEditor::contentChanged);
    QObject::connect(m_pPageMutationHandler, &PageMutationHandler::contentsChanged,
                     this, &NoteEditorPrivate::onContentChanged);

    m_pMimeTypeIconJavaScriptHandler = new MimeTypeIconJavaScriptHandler(m_noteEditorPageFolderPath,
                                                                         m_pIOThread, this);

    QObject::connect(m_pEnCryptElementClickHandler, &EnCryptElementClickHandler::decrypt,
                     this, &NoteEditorPrivate::onEnCryptElementClicked);

    m_pWebChannel->registerObject("resourceCache", m_pResourceLocalFileInfoJavaScriptHandler);
    m_pWebChannel->registerObject("enCryptElementClickHandler", m_pEnCryptElementClickHandler);
    m_pWebChannel->registerObject("pageMutationObserver", m_pPageMutationHandler);
    m_pWebChannel->registerObject("mimeTypeIconHandler", m_pMimeTypeIconJavaScriptHandler);
    QNDEBUG("Registered objects exposed to JavaScript");
}

#endif

void NoteEditorPrivate::setupFileIO()
{
    QNDEBUG("NoteEditorPrivate::setupFileIO");

    m_pIOThread = new QThread;
    QObject::connect(m_pIOThread, QNSIGNAL(QThread,finished), m_pIOThread, QNSLOT(QThread,deleteLater));
    m_pIOThread->start(QThread::LowPriority);

    m_pFileIOThreadWorker = new FileIOThreadWorker;
    m_pFileIOThreadWorker->moveToThread(m_pIOThread);

    QObject::connect(this, QNSIGNAL(NoteEditorPrivate,readDroppedFileData,QString,QUuid),
                     m_pFileIOThreadWorker, QNSLOT(FileIOThreadWorker,onReadFileRequest,QString,QUuid));
    QObject::connect(m_pFileIOThreadWorker, QNSIGNAL(FileIOThreadWorker,readFileRequestProcessed,bool,QString,QByteArray,QUuid),
                     this, QNSLOT(NoteEditorPrivate,onDroppedFileRead,bool,QString,QByteArray,QUuid));
    QObject::connect(this, QNSIGNAL(NoteEditorPrivate,writeNoteHtmlToFile,QString,QByteArray,QUuid),
                     m_pFileIOThreadWorker, QNSLOT(FileIOThreadWorker,onWriteFileRequest,QString,QByteArray,QUuid));
    QObject::connect(m_pFileIOThreadWorker, QNSIGNAL(FileIOThreadWorker,writeFileRequestProcessed,bool,QString,QUuid),
                     this, QNSLOT(NoteEditorPrivate,onWriteFileRequestProcessed,bool,QString,QUuid));

    m_pResourceFileStorageManager = new ResourceFileStorageManager;
    m_pResourceFileStorageManager->moveToThread(m_pIOThread);

    QObject::connect(this, QNSIGNAL(NoteEditorPrivate,saveResourceToStorage,QString,QByteArray,QByteArray,QUuid),
                     m_pResourceFileStorageManager, QNSLOT(ResourceFileStorageManager,onWriteResourceToFileRequest,QString,QByteArray,QByteArray,QUuid));
    QObject::connect(m_pResourceFileStorageManager, QNSIGNAL(ResourceFileStorageManager,writeResourceToFileCompleted,QUuid,QByteArray,int,QString),
                     this, QNSLOT(NoteEditorPrivate,onResourceSavedToStorage,QUuid,QByteArray,int,QString));

}

void NoteEditorPrivate::setupScripts()
{
    QNDEBUG("NoteEditorPrivate::setupScripts");

    __initNoteEditorResources();

    QFile file(":/javascript/jquery/jquery-2.1.3.min.js");
    file.open(QIODevice::ReadOnly);
    m_jQuery = file.readAll();
    file.close();

    file.setFileName(":/javascript/colResizable/colResizable-1.5.min.js");
    file.open(QIODevice::ReadOnly);
    m_resizableColumnsPlugin = file.readAll();
    file.close();

    file.setFileName(":/javascript/scripts/onFixedWidthTableResize.js");
    file.open(QIODevice::ReadOnly);
    m_onFixedWidthTableResize = file.readAll();
    file.close();

    file.setFileName(":/javascript/scripts/getSelectionHtml.js");
    file.open(QIODevice::ReadOnly);
    m_getSelectionHtml = file.readAll();
    file.close();

    file.setFileName(":/javascript/scripts/replaceSelectionWithHtml.js");
    file.open(QIODevice::ReadOnly);
    m_replaceSelectionWithHtml = file.readAll();
    file.close();

    file.setFileName(":/javascript/scripts/provideSrcForResourceImgTags.js");
    file.open(QIODevice::ReadOnly);
    m_provideSrcForResourceImgTags = file.readAll();
    file.close();

    file.setFileName(":/javascript/scripts/enToDoTagsSetup.js");
    file.open(QIODevice::ReadOnly);
    m_setupEnToDoTags = file.readAll();
    file.close();

#ifdef USE_QT_WEB_ENGINE
    file.setFileName(":/qtwebchannel/qwebchannel.js");
    file.open(QIODevice::ReadOnly);
    m_qWebChannelJs = file.readAll();
    file.close();

    file.setFileName(":/javascript/scripts/qWebChannelSetup.js");
    file.open(QIODevice::ReadOnly);
    m_qWebChannelSetupJs = file.readAll();
    file.close();

    file.setFileName(":/javascript/scripts/pageMutationObserver.js");
    file.open(QIODevice::ReadOnly);
    m_pageMutationObserverJs = file.readAll();
    file.close();

    file.setFileName(":/javascript/scripts/provideSrcAndOnClickScriptForEnCryptImgTags.js");
    file.open(QIODevice::ReadOnly);
    m_provideSrcAndOnClickScriptForEnCryptImgTags = file.readAll();
    file.close();

    file.setFileName(":/javascript/scripts/provideSrcForGenericResourceIcons.js");
    file.open(QIODevice::ReadOnly);
    m_provideSrcForGenericResourceIcons = file.readAll();
    file.close();
#endif
}

void NoteEditorPrivate::setupNoteEditorPage()
{
    QNDEBUG("NoteEditorPrivate::setupNoteEditorPage");

    Q_Q(NoteEditor);

    NoteEditorPage * page = new NoteEditorPage(*q);
    page->settings()->setAttribute(WebSettings::LocalContentCanAccessFileUrls, true);
    page->settings()->setAttribute(WebSettings::LocalContentCanAccessRemoteUrls, true);

#ifndef USE_QT_WEB_ENGINE
    page->settings()->setAttribute(QWebSettings::PluginsEnabled, true);
    page->settings()->setAttribute(QWebSettings::DeveloperExtrasEnabled, true);
    page->setContentEditable(true);

    QObject::connect(page, QNSIGNAL(NoteEditorPage,contentsChanged), q, QNSIGNAL(NoteEditor,contentChanged));
    QObject::connect(page, QNSIGNAL(NoteEditorPage,contentsChanged), this, QNSLOT(NoteEditorPrivate,onContentChanged));

    page->mainFrame()->addToJavaScriptWindowObject("resourceCache", m_pResourceLocalFileInfoJavaScriptHandler,
                                                   QScriptEngine::QtOwnership);

    m_pluginFactory = new NoteEditorPluginFactory(*q, *m_pResourceFileStorageManager,
                                                  *m_pFileIOThreadWorker, page);
    page->setPluginFactory(m_pluginFactory);

    EncryptedAreaPlugin * encryptedAreaPlugin = new EncryptedAreaPlugin(m_encryptionManager, m_decryptedTextManager, q);
    m_errorCachedMemory.resize(0);
    NoteEditorPluginFactory::PluginIdentifier encryptedAreaPluginId = m_pluginFactory->addPlugin(encryptedAreaPlugin, m_errorCachedMemory);
    if (!encryptedAreaPluginId) {
        throw NoteEditorPluginInitializationException("Can't initialize note editor plugin for managing the encrypted text");
    }
#endif

    QObject::connect(q, QNSIGNAL(NoteEditor,loadFinished,bool), this, QNSLOT(NoteEditorPrivate,onNoteLoadFinished,bool));
    QObject::connect(this, QNSIGNAL(NoteEditorPrivate,notifyError,QString), q, QNSIGNAL(NoteEditor,notifyError,QString));
    QObject::connect(this, QNSIGNAL(NoteEditorPrivate,convertedToNote,Note), q, QNSIGNAL(NoteEditor,convertedToNote,Note));
    QObject::connect(this, QNSIGNAL(NoteEditorPrivate,cantConvertToNote,QString), q, QNSIGNAL(NoteEditor,cantConvertToNote,QString));
    QObject::connect(this, QNSIGNAL(NoteEditorPrivate,noteEditorHtmlUpdated,QString), q, QNSIGNAL(NoteEditor,noteEditorHtmlUpdated,QString));

    q->setPage(page);
    q->setAcceptDrops(true);
}

void NoteEditorPrivate::setPageEditable(const bool editable)
{
    QNTRACE("NoteEditorPrivate::setPageEditable: " << (editable ? "true" : "false"));

#ifndef USE_QT_WEB_ENGINE
    Q_Q(NoteEditor);
    QWebPage * page = q->page();
    if (Q_LIKELY(page)) {
        page->setContentEditable(editable);
    }
#else
    QString javascript = QString("document.body.contentEditable='") + QString(editable ? "true" : "false") + QString("'; ") +
                         QString("document.designMode='") + QString(editable ? "on" : "off") + QString("'; void 0;");
    m_pJavaScriptInOrderExecutor->append(javascript);
    if (!m_pJavaScriptInOrderExecutor->inProgress()) {
        m_pJavaScriptInOrderExecutor->start();
    }

    QNINFO("Queued javascript to make page " << (editable ? "editable" : "non-editable") << ": " << javascript);
#endif

    m_isPageEditable = editable;
}

void NoteEditorPrivate::onPageHtmlReceived(const QString & html,
                                           const QVector<QPair<QString, QString> > & extraData)
{
    QNDEBUG("NoteEditorPrivate::onPageHtmlReceived");
    Q_UNUSED(extraData)

    emit noteEditorHtmlUpdated(html);

    if (!m_pendingConversionToNote) {
        return;
    }

    if (!m_pNote) {
        m_pendingConversionToNote = false;
        m_errorCachedMemory = QT_TR_NOOP("No current note is set to note editor");
        emit cantConvertToNote(m_errorCachedMemory);
        return;
    }

    m_htmlCachedMemory = html;
    m_enmlCachedMemory.resize(0);
    m_errorCachedMemory.resize(0);
    bool res = m_enmlConverter.htmlToNoteContent(m_htmlCachedMemory, m_enmlCachedMemory,
                                                 m_decryptedTextManager, m_errorCachedMemory);
    if (!res)
    {
        m_errorCachedMemory = QT_TR_NOOP("Can't convert note editor page's content to ENML: ") + m_errorCachedMemory;
        QNWARNING(m_errorCachedMemory)
        emit notifyError(m_errorCachedMemory);

        m_pendingConversionToNote = false;
        emit cantConvertToNote(m_errorCachedMemory);

        return;
    }

    m_pNote->setContent(m_enmlCachedMemory);
    m_pendingConversionToNote = false;
    emit convertedToNote(*m_pNote);
}

void NoteEditorPrivate::onPageSelectedHtmlForEncryptionReceived(const QVariant & selectedHtmlData,
                                                                const QVector<QPair<QString, QString> > & extraData)
{
    QString selectedHtml = selectedHtmlData.toString();

    if (selectedHtml.isEmpty()) {
        QNDEBUG("Note editor page has no selected text, nothing to encrypt");
        return;
    }

    QString passphrase;
    QString hint;
    typedef QVector<QPair<QString,QString> >::const_iterator CIter;
    CIter extraDataEnd = extraData.constEnd();
    for(CIter it = extraData.constBegin(); it != extraDataEnd; ++it)
    {
        const QPair<QString,QString> & itemPair = *it;
        if (itemPair.first == "passphrase") {
            passphrase = itemPair.second;
        }
        else if (itemPair.first == "hint") {
            hint = itemPair.second;
        }
    }

    if (passphrase.isEmpty()) {
        m_errorCachedMemory = QT_TR_NOOP("Internal error: passphrase was either not found within extra data "
                                         "passed along with the selected HTML for encryption or it was passed but is empty");
        QNWARNING(m_errorCachedMemory << ", extra data: " << extraData);
        emit notifyError(m_errorCachedMemory);
        return;
    }

    QString error;
    QString encryptedText;
    QString cipher = "AES";
    size_t keyLength = 128;
    bool res = m_encryptionManager->encrypt(selectedHtml, passphrase, cipher, keyLength,
                                            encryptedText, error);
    if (!res) {
        error.prepend(QT_TR_NOOP("Can't encrypt selected text: "));
        QNWARNING(error);
        emit notifyError(error);
        return;
    }

    QString encryptedTextHtmlObject = "<object type=\"application/octet-stream\" en-tag=\"en-crypt\" >"
                                      "<param name=\"cipher\" value=\"";
    encryptedTextHtmlObject += cipher;
    encryptedTextHtmlObject += "\" />";
    encryptedTextHtmlObject += "<param name=\"length\" value=\"";
    encryptedTextHtmlObject += "\" />";
    encryptedTextHtmlObject += "<param name=\"encryptedText\" value=\"";
    encryptedTextHtmlObject += encryptedText;
    encryptedTextHtmlObject += "\" />";

    if (!hint.isEmpty())
    {
        encryptedTextHtmlObject = "<param name=\"hint\" value=\"";

        QString hintWithEscapedDoubleQuotes = hint;
        for(int i = 0; i < hintWithEscapedDoubleQuotes.size(); ++i)
        {
            if (hintWithEscapedDoubleQuotes.at(i) == QChar('"'))
            {
                if (i == 0) {
                    hintWithEscapedDoubleQuotes.insert(i, QChar('\\'));
                }
                else if (hintWithEscapedDoubleQuotes.at(i-1) != QChar('\\')) {
                    hintWithEscapedDoubleQuotes.insert(i, QChar('\\'));
                }
            }
        }

        encryptedTextHtmlObject += hintWithEscapedDoubleQuotes;
        encryptedTextHtmlObject += "\" />";
    }

    encryptedTextHtmlObject += "<object/>";

#ifndef USE_QT_WEB_ENGINE
    Q_Q(NoteEditor);
    q->page()->mainFrame()->evaluateJavaScript(QString("replaceSelectionWithHtml('%1');").arg(encryptedTextHtmlObject));
    // TODO: see whether contentChanged signal should be emitted manually here
#else
    m_pJavaScriptInOrderExecutor->append(QString("replaceSelectionWithHtml('%1');").arg(encryptedTextHtmlObject));
    if (!m_pJavaScriptInOrderExecutor->inProgress()) {
        m_pJavaScriptInOrderExecutor->start();
    }
#endif
}

#define COMMAND_TO_JS(command) \
    QString javascript = QString("document.execCommand(\"%1\", false, null)").arg(command)

#define COMMAND_WITH_ARGS_TO_JS(command, args) \
    QString javascript = QString("document.execCommand('%1', false, '%2')").arg(command).arg(args)

#ifndef USE_QT_WEB_ENGINE
QVariant NoteEditorPrivate::execJavascriptCommandWithResult(const QString & command)
{
    Q_Q(NoteEditor);
    COMMAND_TO_JS(command);
    QWebFrame * frame = q->page()->mainFrame();
    QVariant result = frame->evaluateJavaScript(javascript);
    QNTRACE("Executed javascript command: " << javascript << ", result = " << result.toString());
    return result;
}

QVariant NoteEditorPrivate::execJavascriptCommandWithResult(const QString & command, const QString & args)
{
    Q_Q(NoteEditor);
    COMMAND_WITH_ARGS_TO_JS(command, args);
    QWebFrame * frame = q->page()->mainFrame();
    QVariant result = frame->evaluateJavaScript(javascript);
    QNTRACE("Executed javascript command: " << javascript << ", result = " << result.toString());
    return result;
}
#endif

void NoteEditorPrivate::execJavascriptCommand(const QString & command)
{
    COMMAND_TO_JS(command);
#ifndef USE_QT_WEB_ENGINE
    Q_Q(NoteEditor);
    QWebFrame * frame = q->page()->mainFrame();
    QVariant result = frame->evaluateJavaScript(javascript);
    QNTRACE("Executed javascript command: " << javascript << ", result = " << result.toString());
#else
    m_pJavaScriptInOrderExecutor->append(javascript);
    if (!m_pJavaScriptInOrderExecutor->inProgress()) {
        m_pJavaScriptInOrderExecutor->start();
    }
#endif
}

void NoteEditorPrivate::execJavascriptCommand(const QString & command, const QString & args)
{
    COMMAND_WITH_ARGS_TO_JS(command, args);
#ifndef USE_QT_WEB_ENGINE
    Q_Q(NoteEditor);
    QWebFrame * frame = q->page()->mainFrame();
    QVariant result = frame->evaluateJavaScript(javascript);
    QNTRACE("Executed javascript command: " << javascript << ", result = " << result.toString());
    return;
#else
    m_pJavaScriptInOrderExecutor->append(javascript);
    if (!m_pJavaScriptInOrderExecutor->inProgress()) {
        m_pJavaScriptInOrderExecutor->start();
    }
#endif
}

void NoteEditorPrivate::setNoteAndNotebook(const Note & note, const Notebook & notebook)
{
    QNDEBUG("NoteEditorPrivate::setNoteAndNotebook: note: local guid = " << note.localGuid()
            << ", guid = " << (note.hasGuid() ? note.guid() : "<null>") << ", title: "
            << (note.hasTitle() ? note.title() : "<null>") << "; notebook: local guid = "
            << notebook.localGuid() << ", guid = " << (notebook.hasGuid() ? notebook.guid() : "<null>")
            << ", name = " << (notebook.hasName() ? notebook.name() : "<null>"));

    if (!m_pNotebook) {
        m_pNotebook = new Notebook(notebook);
    }
    else {
        *m_pNotebook = notebook;
    }

    if (!m_pNote)
    {
        m_pNote = new Note(note);
    }
    else
    {
        if ( (m_pNote->localGuid() == note.localGuid()) &&
             (m_pNote->hasContent() == note.hasContent()) &&
             (!m_pNote->hasContent() || (m_pNote->content() == note.content())) )
        {
            QNDEBUG("This note has already been set for the editor and its content hasn't changed");
        }
        else
        {
            *m_pNote = note;

            m_decryptedTextManager.clearNonRememberedForSessionEntries();
            QNTRACE("Removed non-per-session saved passphrases from decrypted text manager");
        }
    }

#ifndef USE_QT_WEB_ENGINE
    m_pluginFactory->setNote(*m_pNote);
#endif

    noteToEditorContent();
}

const Note * NoteEditorPrivate::getNote()
{
    QNDEBUG("NoteEditorPrivate::getNote");

    if (!m_pNote) {
        QNTRACE("No note was set to the editor");
        return nullptr;
    }

    if (m_modified)
    {
        QNTRACE("Note editor's content was modified, converting into note");

        bool res = htmlToNoteContent(m_errorCachedMemory);
        if (!res) {
            return nullptr;
        }

        m_modified = false;
    }

    return m_pNote;
}

const Notebook * NoteEditorPrivate::getNotebook() const
{
    return m_pNotebook;
}

void NoteEditorPrivate::convertToNote()
{
    QNDEBUG("NoteEditorPrivate::convertToNote");
    m_pendingConversionToNote = true;

    m_errorCachedMemory.resize(0);
    bool res = htmlToNoteContent(m_errorCachedMemory);
    if (!res) {
        m_pendingConversionToNote = false;
        return;
    }
}

bool NoteEditorPrivate::isModified() const
{
    return m_modified;
}

#ifndef USE_QT_WEB_ENGINE
const NoteEditorPluginFactory & NoteEditorPrivate::pluginFactory() const
{
    return *m_pluginFactory;
}

NoteEditorPluginFactory & NoteEditorPrivate::pluginFactory()
{
    return *m_pluginFactory;
}
#endif

void NoteEditorPrivate::onDropEvent(QDropEvent *pEvent)
{
    QNDEBUG("NoteEditorPrivate::onDropEvent");

    if (!pEvent) {
        QNINFO("Null drop event was detected");
        return;
    }

    const QMimeData * pMimeData = pEvent->mimeData();
    if (!pMimeData) {
        QNINFO("Null mime data from drop event was detected");
        return;
    }

    QList<QUrl> urls = pMimeData->urls();
    typedef QList<QUrl>::iterator Iter;
    Iter urlsEnd = urls.end();
    for(Iter it = urls.begin(); it != urlsEnd; ++it)
    {
        QString url = it->toString();
        if (url.toLower().startsWith("file://")) {
            url.remove(0,6);
            dropFile(url);
        }
    }
}

template <typename T>
QString NoteEditorPrivate::composeHtmlTable(const T width, const T singleColumnWidth,
                                            const int rows, const int columns,
                                            const bool relative)
{
    // Table header
    QString htmlTable = "<div><table style=\"border-collapse: collapse; margin-left: 0px; table-layout: fixed; width: ";
    htmlTable += QString::number(width);
    if (relative) {
        htmlTable += "%";
    }
    else {
        htmlTable += "px";
    }
    htmlTable += ";\" ><tbody>";

    for(int i = 0; i < rows; ++i)
    {
        // Row header
        htmlTable += "<tr>";

        for(int j = 0; j < columns; ++j)
        {
            // Column header
            htmlTable += "<td style=\"border: 1px solid rgb(219, 219, 219); padding: 10 px; margin: 0px; width: ";
            htmlTable += QString::number(singleColumnWidth);
            if (relative) {
                htmlTable += "%";
            }
            else {
                htmlTable += "px";
            }
            htmlTable += ";\">";

            // Blank line to preserve the size
            htmlTable += "<div><br></div>";

            // End column
            htmlTable += "</td>";
        }

        // End row
        htmlTable += "</tr>";
    }

    // End table
    htmlTable += "</tbody></table></div>";
    return htmlTable;
}

QString NoteEditorPrivate::attachResourceToNote(const QByteArray & data, const QByteArray & dataHash,
                                                const QMimeType & mimeType, const QString & filename)
{
    QNDEBUG("NoteEditorPrivate::attachResourceToNote: hash = " << dataHash
            << ", mime type = " << mimeType.name());

    if (!m_pNote) {
        QNINFO("Can't attach resource to note editor: no actual note was selected");
        return QString();
    }

    ResourceWrapper resource;
    resource.setDataBody(data);
    resource.setDataHash(dataHash);
    resource.setDataSize(data.size());
    resource.setMime(mimeType.name());
    resource.setDirty(true);

    if (!filename.isEmpty()) {
        qevercloud::ResourceAttributes attributes;
        attributes.fileName = filename;
        resource.setResourceAttributes(attributes);
    }

    m_pNote->addResource(resource);
    return resource.localGuid();
}

void NoteEditorPrivate::insertToDoCheckbox()
{
    QString html = ENMLConverter::getToDoCheckboxHtml(/* checked = */ false);
    QString javascript = QString("document.execCommand('insertHtml', false, '%1'); ").arg(html);
    javascript += m_setupEnToDoTags;

#ifndef USE_QT_WEB_ENGINE
    Q_Q(NoteEditor);
    QWebPage * page = q->page();
    if (Q_UNLIKELY(!page)) {
        return;
    }

    QWebFrame * frame = page->mainFrame();
    if (Q_UNLIKELY(!frame)) {
        return;
    }

    Q_UNUSED(frame->evaluateJavaScript(javascript));
#else
    m_pJavaScriptInOrderExecutor->append(javascript);
    if (!m_pJavaScriptInOrderExecutor->inProgress()) {
        m_pJavaScriptInOrderExecutor->start();
    }
#endif
}

void NoteEditorPrivate::setFont(const QFont & font)
{
    QString fontName = font.family();
    execJavascriptCommand("fontName", fontName);
}

void NoteEditorPrivate::setFontHeight(const int height)
{
    if (height > 0) {
        execJavascriptCommand("fontSize", QString::number(height));
    }
    else {
        QString error = QT_TR_NOOP("Detected incorrect font size: " + QString::number(height));
        QNINFO(error);
        emit notifyError(error);
    }
}

void NoteEditorPrivate::setFontColor(const QColor & color)
{
    if (color.isValid()) {
        execJavascriptCommand("foreColor", color.name());
    }
    else {
        QString error = QT_TR_NOOP("Detected invalid font color: " + color.name());
        QNINFO(error);
        emit notifyError(error);
    }
}

void NoteEditorPrivate::setBackgroundColor(const QColor & color)
{
    if (color.isValid()) {
        execJavascriptCommand("hiliteColor", color.name());
    }
    else {
        QString error = QT_TR_NOOP("Detected invalid background color: " + color.name());
        QNINFO(error);
        emit notifyError(error);
    }
}

void NoteEditorPrivate::insertHorizontalLine()
{
    execJavascriptCommand("insertHorizontalRule");
}

void NoteEditorPrivate::changeIndentation(const bool increase)
{
    execJavascriptCommand((increase ? "indent" : "outdent"));
}

void NoteEditorPrivate::insertBulletedList()
{
    execJavascriptCommand("insertUnorderedList");
}

void NoteEditorPrivate::insertNumberedList()
{
    execJavascriptCommand("insertOrderedList");
}

#define CHECK_NUM_COLUMNS() \
    if (columns <= 0) { \
        QString error = QT_TR_NOOP("Detected attempt to insert table with bad number of columns: ") + QString::number(columns); \
        QNWARNING(error); \
        emit notifyError(error); \
        return; \
    }

#define CHECK_NUM_ROWS() \
    if (rows <= 0) { \
        QString error = QT_TR_NOOP("Detected attempt to insert table with bad number of rows: ") + QString::number(rows); \
        QNWARNING(error); \
        emit notifyError(error); \
        return; \
    }

void NoteEditorPrivate::insertFixedWidthTable(const int rows, const int columns, const int widthInPixels)
{
    CHECK_NUM_COLUMNS();
    CHECK_NUM_ROWS();

    Q_Q(NoteEditor);
    int pageWidth = q->geometry().width();
    if (widthInPixels > 2 * pageWidth) {
        QString error = QT_TR_NOOP("Can't insert table, width is too large (more than twice the page width): ") +
                        QString::number(widthInPixels);
        QNWARNING(error);
        emit notifyError(error);
        return;
    }
    else if (widthInPixels <= 0) {
        QString error = QT_TR_NOOP("Can't insert table, bad width: ") + QString::number(widthInPixels);
        QNWARNING(error);
        emit notifyError(error);
        return;
    }

    int singleColumnWidth = widthInPixels / columns;
    if (singleColumnWidth == 0) {
        QString error = QT_TR_NOOP("Can't insert table, bad width for specified number of columns "
                                   "(single column width is zero): width = ") +
                        QString::number(widthInPixels) + QT_TR_NOOP(", number of columns = ") +
                        QString::number(columns);
        QNWARNING(error);
        emit notifyError(error);
        return;
    }

    QString htmlTable = composeHtmlTable(widthInPixels, singleColumnWidth, rows, columns,
                                         /* relative = */ false);
    execJavascriptCommand("insertHTML", htmlTable);
    updateColResizableTableBindings();
}

void NoteEditorPrivate::insertRelativeWidthTable(const int rows, const int columns, const double relativeWidth)
{
    CHECK_NUM_COLUMNS();
    CHECK_NUM_ROWS();

    if (relativeWidth <= 0.01) {
        QString error = QT_TR_NOOP("Can't insert table, relative width is too small: ") +
                        QString::number(relativeWidth) + QString("%");
        QNWARNING(error);
        emit notifyError(error);
        return;
    }
    else if (relativeWidth > 100.0 +1.0e-9) {
        QString error = QT_TR_NOOP("Can't insert table, relative width is too large: ") +
                        QString::number(relativeWidth) + QString("%");
        QNWARNING(error);
        emit notifyError(error);
        return;
    }

    double singleColumnWidth = relativeWidth / columns;
    QString htmlTable = composeHtmlTable(relativeWidth, singleColumnWidth, rows, columns,
                                         /* relative = */ true);
    execJavascriptCommand("insertHTML", htmlTable);
    updateColResizableTableBindings();
}

void NoteEditorPrivate::encryptSelectedText(const QString & passphrase,
                                            const QString & hint)
{
    QNDEBUG("NoteEditorPrivate::encryptSelectedText");

    Q_Q(NoteEditor);
    QVector<QPair<QString,QString> > extraData;
    extraData << QPair<QString,QString>("passphrase",passphrase) << QPair<QString,QString>("hint",hint);

#ifndef USE_QT_WEB_ENGINE
    if (!q->page()->hasSelection()) {
        QNINFO("Note editor page has no selected text, nothing to encrypt");
        return;
    }

    // NOTE: use JavaScript to get the selected text here, not the convenience method of NoteEditorPage
    // in order to ensure the method returning the selected html and the method replacing the selected html
    // agree about the selected html
    QString selectedHtml = execJavascriptCommandWithResult("getSelectionHtml").toString();
    if (selectedHtml.isEmpty()) {
        QNINFO("Selected html is empty, nothing to encrypt");
        return;
    }

    onPageSelectedHtmlForEncryptionReceived(selectedHtml, extraData);
#else
    q->page()->runJavaScript("getSelectionHtml", HtmlRetrieveFunctor<QVariant>(this, &NoteEditorPrivate::onPageSelectedHtmlForEncryptionReceived, extraData));
#endif
}

void NoteEditorPrivate::onEncryptedAreaDecryption(QString encryptedText, QString decryptedText, bool rememberForSession)
{
    QNDEBUG("NoteEditorPrivate::onEncryptedAreaDecryption");
    Q_UNUSED(encryptedText)
    Q_UNUSED(decryptedText)
    Q_UNUSED(rememberForSession)
    noteToEditorContent();
}

void NoteEditorPrivate::onNoteLoadCancelled()
{
    Q_Q(NoteEditor);
    q->stop();
    QNINFO("Note load has been cancelled");
    // TODO: add some overlay widget for NoteEditor to properly indicate visually that the note load has been cancelled
}

void NoteEditorPrivate::dropFile(QString & filepath)
{
    QNDEBUG("NoteEditorPrivate::dropFile: " << filepath);

    QFileInfo fileInfo(filepath);
    if (!fileInfo.isFile()) {
        QNINFO("Detected attempt to drop something else rather than file: " << filepath);
        return;
    }

    if (!fileInfo.isReadable()) {
        QNINFO("Detected attempt to drop file which is not readable: " << filepath);
        return;
    }

    QMimeDatabase mimeDatabase;
    QMimeType mimeType = mimeDatabase.mimeTypeForFile(fileInfo);

    if (!mimeType.isValid()) {
        QNINFO("Detected invalid mime type for file " << filepath);
        return;
    }

    QUuid readDroppedFileRequestId = QUuid::createUuid();
    auto & pair = m_droppedFileNamesAndMimeTypesByReadRequestIds[readDroppedFileRequestId];
    pair.first = fileInfo.fileName();
    pair.second = mimeType;
    emit readDroppedFileData(filepath, readDroppedFileRequestId);
}

QString ResourceLocalFileInfoJavaScriptHandler::getResourceLocalFilePath(const QString & resourceHash) const
{
    QNTRACE("ResourceLocalFileInfoJavaScriptHandler::getResourceLocalFilePath: " << resourceHash);

    auto it = m_cache.find(resourceHash);
    if (it == m_cache.end()) {
        QNTRACE("Resource local file was not found");
        return QString();
    }

    return it.value();
}

#ifdef USE_QT_WEB_ENGINE

MimeTypeIconJavaScriptHandler::MimeTypeIconJavaScriptHandler(const QString & noteEditorPageFolder,
                                                             QThread * ioThread, QObject * parent) :
    QObject(parent),
    m_noteEditorPageFolder(noteEditorPageFolder),
    m_iconFilePathCache(),
    m_mimeTypeAndLocalFilePathByWriteIconRequestId(),
    m_mimeTypesWithIconsWriteInProgress(),
    m_iconWriter(new FileIOThreadWorker(this))
{
    Q_ASSERT(ioThread);
    m_iconWriter->moveToThread(ioThread);

    QObject::connect(this, QNSIGNAL(MimeTypeIconJavaScriptHandler,writeIconToFile,QString,QByteArray,QUuid),
                     m_iconWriter, QNSLOT(FileIOThreadWorker,onWriteFileRequest,QString,QByteArray,QUuid));
    QObject::connect(m_iconWriter, QNSIGNAL(FileIOThreadWorker,writeFileRequestProcessed,bool,QString,QUuid),
                     this, QNSLOT(MimeTypeIconJavaScriptHandler,onWriteFileRequestProcessed,bool,QString,QUuid));

    QNDEBUG("Initialized MimeTypeIconJavaScriptHandler");
}

void MimeTypeIconJavaScriptHandler::iconFilePathForMimeType(const QString & mimeType)
{
    QNDEBUG("MimeTypeIconJavaScriptHandler::iconFilePathForMimeType: " << mimeType);

    // First try to find it in the cache
    auto it = m_iconFilePathCache.find(mimeType);
    if (it != m_iconFilePathCache.end()) {
        QNTRACE("Found cached icon for mime type " << mimeType << ": " << it.value());
        emit gotIconFilePathForMimeType(mimeType, it.value());
        return;
    }

    // Ok, try to find the already written icon in the appropriate folder
    QString iconsFolderPath = m_noteEditorPageFolder + "/mimeTypeIcons";

    QString normalizedMimeType = mimeType;
    normalizedMimeType.replace('/', '_');

    QString iconFilePath = iconsFolderPath + "/" + normalizedMimeType + ".png";
    QFileInfo mimeTypeIconInfo(iconFilePath);
    if (mimeTypeIconInfo.exists() && mimeTypeIconInfo.isFile() && mimeTypeIconInfo.isReadable()) {
        QNTRACE("Found existing icon written to file: " << iconFilePath);
        QString relativeIconFilePath = relativePath(iconFilePath);
        m_iconFilePathCache[mimeType] = relativeIconFilePath;
        emit gotIconFilePathForMimeType(mimeType, relativeIconFilePath);
        return;
    }

    // If we haven't returned just above, we'd most likely need to return this fallback generic resource icon
    // before we get the proper one written to the local file
    QString fallbackIconFilePath = "qrc:/generic_resource_icons/png/attachment.png";

    // If it's not there, see if we are already in the process of writing this icon to local file
    auto mimeTypeIconWriteInProgressIter = m_mimeTypesWithIconsWriteInProgress.find(mimeType);
    if (mimeTypeIconWriteInProgressIter != m_mimeTypesWithIconsWriteInProgress.end()) {
        QNTRACE("Writing icon for mime type " << mimeType << " is still in progress");
        emit gotIconFilePathForMimeType(mimeType, fallbackIconFilePath);
        return;
    }

    // If it's not there, try to find the icon from theme; if found, schedule the async job
    // of writing that icon to local file
    QMimeDatabase mimeDatabase;
    QMimeType type = mimeDatabase.mimeTypeForName(mimeType);
    QString iconName = type.iconName();
    if (iconName.isEmpty()) {
        iconName = type.genericIconName();
    }

    QIcon icon = QIcon::fromTheme(iconName);
    if (icon.isNull()) {
        QNTRACE("Haven't found the icon corresponding to mime type " << mimeType
                << ", will use the default icon instead");
        m_iconFilePathCache[mimeType] = fallbackIconFilePath;
        emit gotIconFilePathForMimeType(mimeType, fallbackIconFilePath);
        return;
    }

    QImage iconImage = icon.pixmap(QSize(24, 24)).toImage();

    QByteArray iconRawData;
    QBuffer buffer(&iconRawData);
    buffer.open(QIODevice::WriteOnly);
    iconImage.save(&buffer, "png");

    QUuid writeIconRequestId = QUuid::createUuid();
    m_mimeTypeAndLocalFilePathByWriteIconRequestId[writeIconRequestId] = QPair<QString,QString>(mimeType, iconFilePath);
    Q_UNUSED(m_mimeTypesWithIconsWriteInProgress.insert(mimeType));
    emit writeIconToFile(iconFilePath, iconRawData, writeIconRequestId);
    QNTRACE("Emitted a signal to save the icon for mime type " << mimeType
            << " to local file with path " << iconFilePath << ", request id = "
            << writeIconRequestId);
}

void MimeTypeIconJavaScriptHandler::onWriteFileRequestProcessed(bool success, QString errorDescription, QUuid requestId)
{
    auto it = m_mimeTypeAndLocalFilePathByWriteIconRequestId.find(requestId);
    if (it == m_mimeTypeAndLocalFilePathByWriteIconRequestId.end()) {
        return;
    }

    QNDEBUG("MimeTypeIconJavaScriptHandler::onWriteFileRequestProcessed: success = "
            << (success ? "true" : "false") << ", error description = " << errorDescription
            << ", request id = " << requestId);

    QPair<QString,QString> mimeTypeAndFilePath = it.value();
    Q_UNUSED(m_mimeTypeAndLocalFilePathByWriteIconRequestId.erase(it));

    auto mimeTypeIconWriteInProgressIter = m_mimeTypesWithIconsWriteInProgress.find(mimeTypeAndFilePath.first);
    if (mimeTypeIconWriteInProgressIter != m_mimeTypesWithIconsWriteInProgress.end()) {
        Q_UNUSED(m_mimeTypesWithIconsWriteInProgress.erase(mimeTypeIconWriteInProgressIter));
    }

    if (!success) {
        QNWARNING("Can't save resource icon for mime type " << mimeTypeAndFilePath.first
                  << " to local file: " << errorDescription);
        QString fallbackIconFilePath = "qrc:/generic_resource_icons/png/attachment.png";
        m_iconFilePathCache[mimeTypeAndFilePath.first] = fallbackIconFilePath;
        emit gotIconFilePathForMimeType(mimeTypeAndFilePath.first, fallbackIconFilePath);
        return;
    }

    QString relativeIconFilePath = relativePath(mimeTypeAndFilePath.second);
    m_iconFilePathCache[mimeTypeAndFilePath.first] = relativeIconFilePath;
    QNTRACE("Emitting the signal to update icon file path for mime type " << mimeTypeAndFilePath.first);
    emit gotIconFilePathForMimeType(mimeTypeAndFilePath.first, relativeIconFilePath);
}

QString MimeTypeIconJavaScriptHandler::relativePath(const QString & absolutePath) const
{
    int position = absolutePath.indexOf("NoteEditorPage", 0, Qt::CaseInsensitive);
    if (position < 0) {
        return QString();
    }

    return absolutePath.mid(position + 15);
}

#endif

} // namespace qute_note

void __initNoteEditorResources()
{
    Q_INIT_RESOURCE(css);
    Q_INIT_RESOURCE(checkbox_icons);
    Q_INIT_RESOURCE(generic_resource_icons);
    Q_INIT_RESOURCE(jquery);
    Q_INIT_RESOURCE(colResizable);
    Q_INIT_RESOURCE(scripts);

    QNDEBUG("Initialized NoteEditor's resources");
}

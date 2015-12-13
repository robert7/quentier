#ifndef __LIB_QUTE_NOTE__NOTE_EDITOR__DELEGATES__REMOVE_RESOURCE_DELEGATE_H
#define __LIB_QUTE_NOTE__NOTE_EDITOR__DELEGATES__REMOVE_RESOURCE_DELEGATE_H

#include "IUndoableActionDelegate.h"
#include <qute_note/utility/Qt4Helper.h>
#include <qute_note/types/Note.h>
#include <qute_note/types/ResourceWrapper.h>

namespace qute_note {

QT_FORWARD_DECLARE_CLASS(NoteEditorPrivate)
QT_FORWARD_DECLARE_CLASS(FileIOThreadWorker)

class RemoveResourceDelegate: public IUndoableActionDelegate
{
    Q_OBJECT
public:
    explicit RemoveResourceDelegate(const ResourceWrapper & resourceToRemove, NoteEditorPrivate & noteEditor,
                                    FileIOThreadWorker * pFileIOThreadWorker);

    void start();

Q_SIGNALS:
    void finished(ResourceWrapper removedResource, QString htmlWithRemovedResource);
    void notifyError(QString error);

// private signals:
    void writeFile(QString filePath, QByteArray data, QUuid requestId);

private Q_SLOTS:
    void onOriginalPageConvertedToNote(Note note);
    void onResourceReferenceRemovedFromNoteContent(const QVariant & data);
    void onPageHtmlWithoutResourceReceived(const QString & html);

    void onWriteFileRequestProcessed(bool success, QString errorDescription, QUuid requestId);
    void onModifiedPageLoaded();

private:
    void doStart();

private:
    class JsResultCallbackFunctor
    {
    public:
        typedef void (RemoveResourceDelegate::*Method)(const QVariant &);

        JsResultCallbackFunctor(RemoveResourceDelegate & member, Method method) :
            m_member(member),
            m_method(method)
        {}

        void operator()(const QVariant & data) { (m_member.*m_method)(data); }

    private:
        RemoveResourceDelegate &    m_member;
        Method                      m_method;
    };

    class HtmlCallbackFunctor
    {
    public:
        typedef void (RemoveResourceDelegate::*Method)(const QString &);

        HtmlCallbackFunctor(RemoveResourceDelegate & member, Method method) :
            m_member(member),
            m_method(method)
        {}

        void operator()(const QString & html) { (m_member.*m_method)(html); }

    private:
        RemoveResourceDelegate &        m_member;
        Method                          m_method;
    };
private:
    NoteEditorPrivate &             m_noteEditor;
    FileIOThreadWorker *            m_pFileIOThreadWorker;

    ResourceWrapper                 m_resource;
    QString                         m_modifiedHtml;
    QUuid                           m_writeModifiedHtmlToPageSourceRequestId;
};

} // namespace qute_note

#endif // __LIB_QUTE_NOTE__NOTE_EDITOR__DELEGATES__REMOVE_RESOURCE_DELEGATE_H

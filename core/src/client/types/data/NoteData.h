#ifndef __QUTE_NOTE__CORE__CLIENT__TYPES__PRIVATE__NOTE_P_H
#define __QUTE_NOTE__CORE__CLIENT__TYPES__PRIVATE__NOTE_P_H

#include "DataElementWithShortcutData.h"
#include "SynchronizableDataElementData.h"
#include <QEverCloud.h>
#include <QImage>

namespace qute_note {

class NoteData: public DataElementWithShortcutData,
                public SynchronizableDataElementData
{
public:
    NoteData();
    NoteData(const NoteData & other);
    NoteData(NoteData && other);
    NoteData(const qevercloud::Note & other);
    virtual ~NoteData();

    void clear();
    bool checkParameters(QString & errorDescription) const;

    bool containsToDoImpl(const bool checked) const;

    void setContent(const QString & content);

    qevercloud::Note m_qecNote;

    struct ResourceAdditionalInfo
    {
        QString  localGuid;
        QString  noteLocalGuid;
        bool     isDirty;

        bool operator==(const ResourceAdditionalInfo & other) const;
    };

    QList<ResourceAdditionalInfo> m_resourcesAdditionalInfo;
    bool     m_isLocal;
    QImage   m_thumbnail;

    mutable QString  m_lazyPlainText;
    mutable bool     m_lazyPlainTextIsValid;

    mutable QStringList  m_lazyListOfWords;
    mutable bool         m_lazyListOfWordsIsValid;

    // these flags are int in order to handle the "empty" state:
    // "-1" - empty, undefined
    // "0" - false
    // "1" - true
    mutable int  m_lazyContainsCheckedToDo;
    mutable int  m_lazyContainsUncheckedToDo;
    mutable int  m_lazyContainsEncryption;

private:
    NoteData & operator=(const NoteData & other) = delete;
    NoteData & operator=(NoteData && other) = delete;
};

} // namespace qute_note

#endif // __QUTE_NOTE__CORE__CLIENT__TYPES__PRIVATE__NOTE_P_H

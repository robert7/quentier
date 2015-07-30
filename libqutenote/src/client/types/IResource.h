#ifndef __QUTE_NOTE__CLIENT__TYPES__IRESOURCE_H
#define __QUTE_NOTE__CLIENT__TYPES__IRESOURCE_H

#include "INoteStoreDataElement.h"
#include "Note.h"
#include <QEverCloud.h>

namespace qute_note {

QT_FORWARD_DECLARE_CLASS(NoteStoreDataElementData)

class QUTE_NOTE_EXPORT IResource: public INoteStoreDataElement
{
public:
    QN_DECLARE_LOCAL_GUID
    QN_DECLARE_DIRTY
    QN_DECLARE_LOCAL

public:
    IResource();
    IResource(const bool isFreeAccount);
    virtual ~IResource();

    bool operator==(const IResource & other) const;
    bool operator!=(const IResource & other) const;

    operator const qevercloud::Resource&() const;
    operator qevercloud::Resource&();

    virtual void clear() Q_DECL_OVERRIDE;

    virtual bool hasGuid() const Q_DECL_OVERRIDE;
    virtual const QString & guid() const Q_DECL_OVERRIDE;
    virtual void setGuid(const QString & guid) Q_DECL_OVERRIDE;

    virtual bool hasUpdateSequenceNumber() const Q_DECL_OVERRIDE;
    virtual qint32 updateSequenceNumber() const Q_DECL_OVERRIDE;
    virtual void setUpdateSequenceNumber(const qint32 updateSequenceNumber) Q_DECL_OVERRIDE;

    virtual bool checkParameters(QString & errorDescription) const Q_DECL_OVERRIDE;

    bool isFreeAccount() const;
    void setFreeAccount(const bool isFreeAccount);

    int indexInNote() const;
    void setIndexInNote(const int index);

    bool hasNoteGuid() const;
    const QString & noteGuid() const;
    void setNoteGuid(const QString & guid);

    bool hasNoteLocalGuid() const;
    const QString & noteLocalGuid() const;
    void setNoteLocalGuid(const QString & guid);

    bool hasData() const;

    bool hasDataHash() const;
    const QByteArray & dataHash() const;
    void setDataHash(const QByteArray & hash);

    bool hasDataSize() const;
    qint32 dataSize() const;
    void setDataSize(const qint32 size);

    bool hasDataBody() const;
    const QByteArray & dataBody() const;
    void setDataBody(const QByteArray & body);

    bool hasMime() const;
    const QString & mime() const;
    void setMime(const QString & mime);

    bool hasWidth() const;
    qint16 width() const;
    void setWidth(const qint16 width);

    bool hasHeight() const;
    qint16 height() const;
    void setHeight(const qint16 height);

    bool hasRecognitionData() const;

    bool hasRecognitionDataHash() const;
    const QByteArray & recognitionDataHash() const;
    void setRecognitionDataHash(const QByteArray & hash);

    bool hasRecognitionDataSize() const;
    qint32 recognitionDataSize() const;
    void setRecognitionDataSize(const qint32 size);

    bool hasRecognitionDataBody() const;
    const QByteArray & recognitionDataBody() const;
    void setRecognitionDataBody(const QByteArray & body);

    bool hasRecognitionType(const QString & recognitionType) const;
    const QStringList recognitionTypes() const;

    bool hasAlternateData() const;

    bool hasAlternateDataHash() const;
    const QByteArray & alternateDataHash() const;
    void setAlternateDataHash(const QByteArray & hash);

    bool hasAlternateDataSize() const;
    qint32 alternateDataSize() const;
    void setAlternateDataSize(const qint32 size);

    bool hasAlternateDataBody() const;
    const QByteArray & alternateDataBody() const;
    void setAlternateDataBody(const QByteArray & body);

    bool hasResourceAttributes() const;
    const qevercloud::ResourceAttributes & resourceAttributes() const;
    qevercloud::ResourceAttributes & resourceAttributes();
    void setResourceAttributes(const qevercloud::ResourceAttributes & attributes);
    void setResourceAttributes(qevercloud::ResourceAttributes && attributes);

    friend class Note;
    friend class ResourceWrapper;

protected:
    IResource(const IResource & other);
    IResource & operator=(const IResource & other);

    virtual const qevercloud::Resource & GetEnResource() const = 0;
    virtual qevercloud::Resource & GetEnResource() = 0;

    virtual QTextStream & Print(QTextStream & strm) const;

private:
    QSharedDataPointer<NoteStoreDataElementData>  d;

    bool m_isFreeAccount;
    int  m_indexInNote;
    qevercloud::Optional<QString> m_noteLocalGuid;
    mutable QStringList m_lazyRecognitionType;
};

} // namespace qute_note

#endif // __QUTE_NOTE__CLIENT__TYPES__IRESOURCE_H
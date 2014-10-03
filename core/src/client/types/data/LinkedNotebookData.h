#ifndef __QUTE_NOTE__CORE__CLIENT__TYPES__DATA__LINKED_NOTEBOOK_H
#define __QUTE_NOTE__CORE__CLIENT__TYPES__DATA__LINKED_NOTEBOOK_H

#include "NoteStoreDataElementData.h"
#include <QEverCloud.h>

namespace qute_note {

class LinkedNotebookData : public NoteStoreDataElementData
{
public:
    LinkedNotebookData();
    LinkedNotebookData(const LinkedNotebookData & other);
    LinkedNotebookData(LinkedNotebookData && other);
    LinkedNotebookData(const qevercloud::LinkedNotebook & other);
    LinkedNotebookData(qevercloud::LinkedNotebook && other);
    virtual ~LinkedNotebookData();

    void clear();
    bool checkParameters(QString & errorDescription) const;

    bool operator==(const LinkedNotebookData & other) const;
    bool operator!=(const LinkedNotebookData & other) const;

    qevercloud::LinkedNotebook    m_qecLinkedNotebook;

private:
    LinkedNotebookData & operator=(const LinkedNotebookData & other) = delete;
    LinkedNotebookData & operator=(LinkedNotebookData && other) = delete;
};

} // namespace qute_note

#endif // __QUTE_NOTE__CORE__CLIENT__TYPES__DATA__LINKED_NOTEBOOK_H

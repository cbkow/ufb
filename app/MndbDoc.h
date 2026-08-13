// MndbDoc — QML singleton rendering a minNotes document (.mndb, a
// SQLite file: blocks + doc_meta) into a self-contained HTML file for
// the lightbox's HtmlPreview (WebEngine). UFB reads the blocks table
// directly — the user chose this over a minNotes-side stored preview,
// accepting an approximate look that works on every existing document.
//
// Block storage contract (mirrors minNotes app/core/BlockModel.cpp,
// verified 2026-08-13): blocks(type TEXT, attrs JSON, content TEXT,
// depth INT) ordered by rank; type strings paragraph/heading/code/
// media/quote/list_item/task_item/ordered_item/divider/table; attrs
// keys level (1-6), state (task 0/1/2), lang; inline formatting is
// markdown stored verbatim in content; media content is a JSON
// descriptor whose "src" is ".minnotes/<sha>.<ext>" (doc-relative),
// an absolute path, or a portable {vol,rel} object (skipped v1);
// table content is grid JSON {cols, header, rows:[[...]]}.

#pragma once

#include <QObject>
#include <QString>
#include <QtQmlIntegration>

class MndbDoc : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit MndbDoc(QObject *parent = nullptr) : QObject(parent) {}

    // Renders `mndbPath` to a temp HTML file and returns its path, or
    // "" when the document can't be opened/read (HtmlPreview then never
    // routes — the lightbox falls back to the file icon). Synchronous;
    // a typical document is a few hundred small rows.
    Q_INVOKABLE QString htmlPreviewPath(const QString &mndbPath) const;
};

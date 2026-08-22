// MndbDoc — QML singleton rendering a minNotes document (.mndb, a
// SQLite file: blocks + doc_meta — or a .mnpkg package: a zip of
// document.mndb + media/, staged to a temp dir first) into a
// self-contained HTML file for the lightbox's HtmlPreview (WebEngine). UFB reads the blocks table
// directly — the user chose this over a minNotes-side stored preview,
// accepting an approximate look that works on every existing document.
//
// Block storage contract (mirrors minNotes app/core/BlockModel.cpp +
// TableGrid.cpp, verified 2026-08-22 against minNotes e5b6c07 / schema
// v3): blocks(type TEXT, attrs JSON, content TEXT, depth INT) ordered by
// rank; type strings paragraph/heading/code/media/quote/list_item/
// task_item/ordered_item/divider/table; attrs keys level (1-6), state
// (task 0/1/2), lang, spans [{s,e,k,u}] with STRING kinds (bold/italic/
// code/strike/underline/link/color/highlight/comment/choice; choice u =
// {"o":[{id,l,c}],"v":id} JSON-in-string, span text = the label); media
// content is a JSON descriptor whose "src" is ".minnotes/<sha>.<ext>"
// (doc-relative), an absolute path, or a portable {vol,rel} object
// (unresolved → reference figure); sketches carry shapes/images/texts
// inline; table content is grid JSON {cols:N, header, w, a, rbg/rfg/
// cbg/cfg, ct:{col:{k:1 choice|2 check, o}}, rows:[[cell…]]} where a
// cell is a string or {t,bg,fg,s (INTEGER span kinds),m (media JSON
// string),v}; block_ink rows carry margin strokes + text chips;
// doc_meta.page_width (v3, default 760) is the measure.

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

    // Renders `mndbPath` (.mndb, or .mnpkg) to a temp HTML file and returns its path, or
    // "" when the document can't be opened/read (HtmlPreview then never
    // routes — the lightbox falls back to the file icon). Synchronous;
    // a typical document is a few hundred small rows.
    Q_INVOKABLE QString htmlPreviewPath(const QString &mndbPath) const;
};

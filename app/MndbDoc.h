// MndbDoc — QML singleton rendering a minNotes document into a
// self-contained HTML file for the lightbox's HtmlPreview (WebEngine).
// Inputs: .mnd (minNotes 1.0+), .mndb (pre-1.0 — minNotes itself refuses
// these since the 1.0 clean break, so this preview is where they still
// read), and .mnpkg packages (a zip of document.mnd|document.mndb +
// media/, staged to a temp dir first). All are SQLite: blocks + doc_meta.
// UFB reads the blocks table directly — the user chose this over a
// minNotes-side stored preview, accepting an approximate look that works
// on every existing document. The preview never gates on doc_meta.format
// or schema_version (1.0 restarted the numbering at 1 under format
// "mnd"); it renders whichever block contract the rows carry.
//
// Block storage contract (mirrors minNotes app/core/BlockModel.cpp,
// verified 2026-09-17 against minNotes 2eb969a / v1.0.2): blocks(type
// TEXT, attrs JSON|NULL, content TEXT, depth INT) ordered by rank; type
// strings paragraph/heading/code/media/quote/list_item/task_item/
// ordered_item/divider/split (+ the pre-1.0 `table`); attrs keys level
// (1-6), state (task 0/1/2), lang, cell (lane/column — see below), spans
// [{s,e,k,u}] with STRING kinds (bold/italic/code/strike/underline/link/
// color/highlight/comment/choice; choice u = {"o":[{id,l,c}],"v":id}
// JSON-in-string, span text = the label); media content is a JSON
// descriptor whose "src" is ".minnotes/<sha>.<ext>" (doc-relative), an
// absolute path, or a portable {vol,rel} object (unresolved → reference
// figure); sketches carry shapes/images/texts inline; block_ink rows
// carry margin strokes + text chips (the anchor may be a split record);
// doc_meta.page_width (default 760) is the measure.
//
// 1.0 structure — split rows and DERIVED tables (no table block exists):
// a `split` record (content '') is followed in rank order by its lane
// blocks, each with attrs.cell = k; attrs.ratios are the lane shares. A
// record with attrs.header >= 1 heads a table whose rows are the records
// directly below it; attrs.table carries column specs and colors. Full
// notes sit with the renderer in MndbDoc.cpp.
//
// Pre-1.0 (.mndb) tables: one `table` block whose content is grid JSON
// {cols:N, header, w, a, rbg/rfg/cbg/cfg, ct:{col:{k:1 choice|2 check,
// o}}, rows:[[cell…]]} where a cell is a string or {t,bg,fg,s (INTEGER
// span kinds),m (media JSON string),v}.

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

    // Renders `mndbPath` (.mnd, .mndb or .mnpkg) to a temp HTML file and returns its path, or
    // "" when the document can't be opened/read (HtmlPreview then never
    // routes — the lightbox falls back to the file icon). Synchronous;
    // a typical document is a few hundred small rows.
    Q_INVOKABLE QString htmlPreviewPath(const QString &mndbPath) const;
};

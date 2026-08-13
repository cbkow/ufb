// TextInfo — QML singleton backing the lightbox's text preview. Two jobs:
//
//  1. isText(path): should this file route to TextPreview? Known text
//     extensions answer instantly; unknown extensions (README, dotfiles,
//     exported logs with odd suffixes) fall through to a bounded 4 KB
//     content sniff, so extensionless text previews without ever routing
//     binaries to the text pane. Media extensions never reach the sniff.
//
//  2. readHead(path): the capped read TextPreview renders. Replaces the
//     old file:// XMLHttpRequest, which materialised the ENTIRE file in
//     RAM before the QML side truncated to 1 MB — a multi-GB render log
//     would spike memory. This reads at most the cap from disk.
//
// Mirrors ExrInfo/PdfDoc: tiny synchronous singleton, preview-open use
// only (one 4 KB read worst-case on the QML thread per Space press).

#pragma once

#include <QObject>
#include <QString>
#include <QtQmlIntegration>

class TextInfo : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit TextInfo(QObject *parent = nullptr) : QObject(parent) {}

    // True when `path` should open in the text preview. Extension gate
    // first (known text → true, known media → false), content sniff for
    // the long tail of unknown extensions and extensionless files.
    Q_INVOKABLE bool isText(const QString &path) const;

    // First `maxBytes` of the file decoded as UTF-8 (invalid sequences
    // become U+FFFD), with a truncation marker appended when the file is
    // larger than the cap. Empty string on open failure.
    Q_INVOKABLE QString readHead(const QString &path,
                                 int maxBytes = 1000000) const;
};

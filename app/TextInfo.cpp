#include "TextInfo.h"
#include "thumbnails/Thumbnailer.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>

// Extensions that route straight to the text preview, no sniff needed.
// Plain text + markup + config + subtitles + the code formats that show
// up in job folders. svg/xml-based media formats are deliberately absent
// — they belong to the image pipeline (Thumbnailer wins below).
static const QSet<QString>& textExts() {
    static const QSet<QString> s {
        // plain / docs
        "txt", "text", "md", "markdown", "log", "nfo", "tex", "rst", "adoc",
        // rendered rich text (TextPreview switches textFormat on these)
        "html", "htm",
        // data / config
        "json", "json5", "jsonl", "ndjson", "xml", "csv", "tsv",
        "ini", "cfg", "conf", "config", "yaml", "yml", "toml",
        "properties", "env", "plist",
        // subtitles
        "srt", "vtt", "ass", "ssa", "sub",
        // code — the point is "don't show an icon for something readable",
        // not an exhaustive language registry.
        "py", "pyw", "js", "mjs", "cjs", "ts", "tsx", "jsx", "sh", "zsh",
        "bash", "bat", "cmd", "ps1", "rb", "pl", "pm", "lua", "rs", "go",
        "java", "kt", "swift", "c", "h", "cpp", "hpp", "cc", "hh", "cxx",
        "m", "mm", "cs", "css", "scss", "sass", "less", "sql", "r", "php",
        "qml", "cmake", "gradle", "diff", "patch",
    };
    return s;
}

// Bounded content sniff for the unknown-extension tail. Text means: no
// NUL bytes and almost no control characters outside tab/newline/return/
// formfeed in the first 4 KB. High bytes (0x80+) pass — UTF-8 multibyte
// and Latin-1 both read fine downstream (invalid UTF-8 renders as U+FFFD,
// still legible). Empty files count as text (preview shows empty).
static bool sniffLooksText(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(4096);
    if (head.isEmpty()) return true;
    int control = 0;
    for (unsigned char c : head) {
        if (c == 0x00) return false;
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f')
            ++control;
    }
    return control * 20 < head.size();  // >5% control bytes → binary
}

bool TextInfo::isText(const QString &path) const {
    if (path.isEmpty()) return false;
    const QString ext = QFileInfo(path).suffix().toLower();
    if (textExts().contains(ext)) return true;
    // Anything the thumbnail pipeline claims (stills, video, PDF, RAW,
    // Office, …) is media — never sniff it. Cheap pure set lookups.
    Thumbnailer t;
    if (t.mayHaveThumbnail(path)) return false;
    return sniffLooksText(path);
}

QString TextInfo::readHead(const QString &path, int maxBytes) const {
    if (path.isEmpty() || maxBytes <= 0) return QString();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const QByteArray head = f.read(maxBytes);
    QString out = QString::fromUtf8(head);
    if (f.size() > head.size())
        out += QStringLiteral("\n\n… (truncated — showing first %1 of %2 bytes)")
                   .arg(head.size()).arg(f.size());
    return out;
}

#include "MndbDoc.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>

namespace {

QString escapeHtml(QString s) {
    s.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    s.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    s.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return s;
}

// Conservative inline-markdown pass over ALREADY-ESCAPED text: `code`,
// **bold**, *italic*, [text](url). Not a real parser — a preview
// approximation of the same verbatim markdown minNotes renders.
QString inlineMd(QString s) {
    static const QRegularExpression code(QStringLiteral("`([^`]+)`"));
    static const QRegularExpression bold(QStringLiteral("\\*\\*([^*]+)\\*\\*"));
    static const QRegularExpression italic(QStringLiteral("\\*([^*]+)\\*"));
    static const QRegularExpression link(QStringLiteral("\\[([^\\]]+)\\]\\(([^)\\s]+)\\)"));
    s.replace(code, QStringLiteral("<code>\\1</code>"));
    s.replace(bold, QStringLiteral("<b>\\1</b>"));
    s.replace(italic, QStringLiteral("<i>\\1</i>"));
    s.replace(link, QStringLiteral("<a href=\"\\2\">\\1</a>"));
    s.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    return s;
}

QString inlineHtml(const QString& raw) { return inlineMd(escapeHtml(raw)); }

// Media descriptor "src" → absolute local path ("" when unresolvable,
// e.g. the portable {vol,rel} form we don't map in v1).
QString resolveMediaSrc(const QJsonValue& src, const QString& docDir) {
    if (!src.isString()) return {};
    const QString s = src.toString();
    if (s.startsWith(QLatin1String(".minnotes/")))
        return docDir + QLatin1Char('/') + s;
    if (s.startsWith(QLatin1String("http")))
        return {};                        // remote — skip in preview
    return s;                             // absolute reference
}

QString mediaHtml(const QString& content, const QString& docDir) {
    const QJsonObject o = QJsonDocument::fromJson(content.toUtf8()).object();
    const QString kind = o.value(QStringLiteral("kind")).toString();
    const QString abs = resolveMediaSrc(o.value(QStringLiteral("src")), docDir);
    const QString name = escapeHtml(QFileInfo(abs).fileName());

    const bool isImage = kind == QLatin1String("image") || kind == QLatin1String("sketch")
        || (kind.isEmpty() && !abs.isEmpty());  // old image blocks may omit kind
    if (isImage && !abs.isEmpty() && QFileInfo::exists(abs)) {
        return QStringLiteral("<figure><img src=\"%1\" alt=\"%2\"></figure>")
            .arg(QUrl::fromLocalFile(abs).toString(QUrl::FullyEncoded), name);
    }
    // Video / PDF / unresolvable → an honest chip, not a broken image.
    const QString glyph = kind == QLatin1String("video") ? QStringLiteral("&#9654;")
                        : kind == QLatin1String("pdf")   ? QStringLiteral("&#128196;")
                                                         : QStringLiteral("&#128279;");
    const QString label = name.isEmpty() ? QStringLiteral("(unavailable media)") : name;
    return QStringLiteral("<div class=\"chip\">%1 %2</div>").arg(glyph, label);
}

QString tableHtml(const QString& gridJson) {
    const QJsonObject o = QJsonDocument::fromJson(gridJson.toUtf8()).object();
    const QJsonArray rows = o.value(QStringLiteral("rows")).toArray();
    const int header = o.value(QStringLiteral("header")).toInt(0);
    QString out = QStringLiteral("<table>");
    for (int i = 0; i < rows.size(); ++i) {
        const QJsonArray cells = rows.at(i).toArray();
        const bool th = i < header;
        out += QStringLiteral("<tr>");
        for (const QJsonValue& c : cells)
            out += QStringLiteral("<%1>%2</%1>")
                       .arg(th ? QStringLiteral("th") : QStringLiteral("td"),
                            inlineHtml(c.toString()));
        out += QStringLiteral("</tr>");
    }
    return out + QStringLiteral("</table>");
}

// The preview's whole look — a lean take on the minNotes export theme
// (dark worksheet, left-anchored 760 measure, blue accent).
const char* kCss =
    "body{background:#181817;color:#e4e3e2;margin:0;"
    "font:15px/1.65 -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
    "main{max-width:760px;margin:0 auto;padding:40px 24px 96px}"
    "h1,h2,h3,h4,h5,h6{color:#f0f0f0;line-height:1.25}"
    "a{color:#0189f1;text-decoration:none}"
    "blockquote{border-left:3px solid #3a5e86;margin:0;padding:2px 0 2px 14px;color:#b8c4d4}"
    "pre{background:#0e0e0e;border:1px solid #2a2a2a;border-radius:4px;"
    "padding:12px 14px;overflow-x:auto;font:12.5px/1.5 ui-monospace,Menlo,Consolas,monospace;"
    "color:#d4d4e8}"
    "code{font:0.92em ui-monospace,Menlo,Consolas,monospace;background:#0e0e0e;"
    "padding:1px 5px;border-radius:3px;color:#d4d4e8}"
    "pre code{background:none;padding:0}"
    "hr{border:none;border-top:1px solid #333;margin:24px 0}"
    "table{border-collapse:collapse;margin:8px 0}"
    "th,td{border:1px solid #2a2a2a;padding:6px 12px;text-align:left}"
    "th{color:#f0f0f0;background:#1d1d1c}"
    "figure{margin:12px 0}img{max-width:100%;display:block}"
    ".li{margin:2px 0}.chip{display:inline-block;background:#1d2733;color:#4aa8ff;"
    "border-radius:4px;padding:4px 10px;margin:6px 0;font-size:13px}"
    ".title{font-size:26px;font-weight:700;color:#f0f0f0;margin-bottom:24px}";

}  // namespace

QString MndbDoc::htmlPreviewPath(const QString& mndbPath) const {
    if (mndbPath.isEmpty() || !QFileInfo::exists(mndbPath)) return {};
    const QString docDir = QFileInfo(mndbPath).absolutePath();

    // Unique connection per call; read-only so a doc open in minNotes
    // (or on SMB without shm backing) is never disturbed.
    const QString conn = QStringLiteral("mndb-preview-%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            mndbPath.toUtf8(), QCryptographicHash::Sha1).toHex().left(12)));
    QString body;
    QString title;
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(mndbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery meta(db);
            if (meta.exec(QStringLiteral("SELECT title FROM doc_meta WHERE id=1"))
                && meta.next())
                title = meta.value(0).toString();

            QSqlQuery q(db);
            if (q.exec(QStringLiteral(
                    "SELECT type, attrs, content, depth FROM blocks ORDER BY rank"))) {
                ok = true;
                int orderedN = 0;
                while (q.next()) {
                    const QString type = q.value(0).toString();
                    const QJsonObject attrs = QJsonDocument::fromJson(
                        q.value(1).toString().toUtf8()).object();
                    const QString content = q.value(2).toString();
                    const int depth = q.value(3).toInt();
                    const bool ordered = type == QLatin1String("ordered_item");
                    if (!ordered) orderedN = 0;

                    if (type == QLatin1String("heading")) {
                        const int lv = qBound(1, attrs.value(QStringLiteral("level")).toInt(1), 6);
                        body += QStringLiteral("<h%1>%2</h%1>").arg(lv).arg(inlineHtml(content));
                    } else if (type == QLatin1String("quote")) {
                        body += QStringLiteral("<blockquote>%1</blockquote>").arg(inlineHtml(content));
                    } else if (type == QLatin1String("code")) {
                        body += QStringLiteral("<pre><code>%1</code></pre>").arg(escapeHtml(content));
                    } else if (type == QLatin1String("divider")) {
                        body += QStringLiteral("<hr>");
                    } else if (type == QLatin1String("table")) {
                        body += tableHtml(content);
                    } else if (type == QLatin1String("media")) {
                        body += mediaHtml(content, docDir);
                    } else if (type == QLatin1String("list_item")
                               || type == QLatin1String("task_item")
                               || type == QLatin1String("ordered_item")) {
                        QString bullet = QStringLiteral("&bull;");
                        if (ordered)
                            bullet = QStringLiteral("%1.").arg(++orderedN);
                        else if (type == QLatin1String("task_item")) {
                            const int st = attrs.value(QStringLiteral("state")).toInt(0);
                            bullet = st == 2 ? QStringLiteral("&#9745;")   // done
                                   : st == 1 ? QStringLiteral("&#9686;")   // doing
                                             : QStringLiteral("&#9744;");  // todo
                        }
                        body += QStringLiteral(
                            "<div class=\"li\" style=\"margin-left:%1px\">%2 %3</div>")
                            .arg(qBound(0, depth, 8) * 22).arg(bullet, inlineHtml(content));
                    } else {  // paragraph + unknown future types degrade to text
                        body += content.isEmpty()
                            ? QStringLiteral("<p>&nbsp;</p>")
                            : QStringLiteral("<p>%1</p>").arg(inlineHtml(content));
                    }
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    if (!ok) return {};

    QString html = QStringLiteral("<!doctype html><meta charset=\"utf-8\"><style>%1</style><main>")
        .arg(QString::fromLatin1(kCss));
    if (!title.isEmpty())
        html += QStringLiteral("<div class=\"title\">%1</div>").arg(escapeHtml(title));
    html += body + QStringLiteral("</main>");

    const QString out = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/ufb-mndb-%1.html")
              .arg(QString::fromLatin1(QCryptographicHash::hash(
                  mndbPath.toUtf8(), QCryptographicHash::Sha1).toHex().left(16)));
    QFile f(out);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(html.toUtf8());
    f.close();
    return out;
}

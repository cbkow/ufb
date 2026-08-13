#include "MndbDoc.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace {

QString escapeHtml(QString s) {
    s.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    s.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    s.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    // Quotes too — escaped values land inside double-quoted attributes
    // (href/style/alt), where an unescaped quote breaks out of the
    // attribute and doc data becomes markup.
    s.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    s.replace(QLatin1Char('\''), QStringLiteral("&#39;"));
    return s;
}

// Hrefs come straight from doc data, and the preview runs with JS on in
// a file:// origin — a javascript:/data: href is script execution.
// Scheme-sniff a control-stripped copy (Chromium strips those chars
// before resolving, so "java\nscript:" would slip a naive check).
bool hrefSchemeOk(const QString& u) {
    QString probe = u;
    static const QRegularExpression ctl(QStringLiteral("[\\x00-\\x20]"));
    probe.remove(ctl);
    const QString scheme = QUrl(probe).scheme().toLower();
    return scheme.isEmpty()
        || scheme == QLatin1String("http")
        || scheme == QLatin1String("https")
        || scheme == QLatin1String("mailto")
        || scheme == QLatin1String("file");
}

QString safeHrefAttr(const QString& u) {
    return hrefSchemeOk(u) ? escapeHtml(u) : QStringLiteral("#");
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
    // Linkify per-match (not a blind replace) so scriptable schemes
    // stay plain text. The url is already entity-escaped; scheme
    // characters are untouched by escaping, so the check still works.
    {
        QString rebuilt;
        qsizetype last = 0;
        auto it = link.globalMatch(s);
        while (it.hasNext()) {
            const auto m = it.next();
            rebuilt += s.mid(last, m.capturedStart() - last);
            if (hrefSchemeOk(m.captured(2)))
                rebuilt += QStringLiteral("<a href=\"%1\">%2</a>")
                               .arg(m.captured(2), m.captured(1));
            else
                rebuilt += m.captured(0);
            last = m.capturedEnd();
        }
        rebuilt += s.mid(last);
        s = rebuilt;
    }
    s.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    return s;
}

QString inlineHtml(const QString& raw) { return inlineMd(escapeHtml(raw)); }

// ── Span-based inline formatting — ported from minNotes ────────────────
// (Exporter.cpp emitInlineHtml). v1+ docs store CLEAN text plus
// attrs.spans [{s,e,k,u}] with string kinds; the markdown pass above is
// only the fallback for span-less blocks. The walk splits the text at
// every span boundary and keeps a tag stack ordered by rank, so
// overlapping spans nest deterministically.

// Highlight text needs contrast against the user's swatch, chosen by luma.
QString contrastOn(const QString& hex) {
    const QColor c(hex);
    if (!c.isValid()) return QStringLiteral("#f0f0f0");
    const double luma = 0.299 * c.red() + 0.587 * c.green() + 0.114 * c.blue();
    return luma > 140.0 ? QStringLiteral("#111111") : QStringLiteral("#f0f0f0");
}

int spanRank(const QString& k) {
    if (k == QLatin1String("comment"))   return 0;   // outermost
    if (k == QLatin1String("link"))      return 1;
    if (k == QLatin1String("color"))     return 2;
    if (k == QLatin1String("highlight")) return 3;
    if (k == QLatin1String("bold"))      return 4;
    if (k == QLatin1String("italic"))    return 5;
    if (k == QLatin1String("strike"))    return 6;
    if (k == QLatin1String("underline")) return 7;
    if (k == QLatin1String("code"))      return 8;
    return 9;                                        // unknown → dropped
}

// Comment bookkeeping shared between the span walk (anchors + hover
// cards) and the trailing comments section.
struct CommentCtx {
    QHash<QString, int> num;              // thread id → 1-based number
    QStringList order;                    // numbering order
    QHash<QString, QString> sectionMsgs;  // div-wrapped, for the section
    QHash<QString, QString> cardMsgs;     // span-wrapped, for hover cards
    QHash<QString, bool> resolved;
    bool known(const QString& tid) const {
        return resolved.contains(tid) || sectionMsgs.contains(tid);
    }
    int numberFor(const QString& tid) {
        auto it = num.find(tid);
        if (it != num.end()) return *it;
        num.insert(tid, num.size() + 1);
        order.append(tid);
        return num.size();
    }
};

QString spansHtml(const QString& text, const QJsonArray& spans, CommentCtx& cc) {
    const int len = int(text.size());

    struct Run {
        int s, e, rank; QString k, u; int note = 0;
        bool operator==(const Run& o) const {
            return k == o.k && u == o.u && s == o.s && e == o.e;
        }
    };
    std::vector<Run> runs;
    for (const QJsonValue& v : spans) {
        const QJsonObject sp = v.toObject();
        const QString k = sp.value(QStringLiteral("k")).toString();
        const int rank = spanRank(k);
        if (rank > 8) continue;
        const int s = std::clamp(sp.value(QStringLiteral("s")).toInt(), 0, len);
        const int e = std::clamp(sp.value(QStringLiteral("e")).toInt(), 0, len);
        if (s >= e) continue;
        Run r{s, e, rank, k, sp.value(QStringLiteral("u")).toString(), 0};
        if (k == QLatin1String("comment")) {
            if (!cc.known(r.u)) continue;   // orphaned anchor → plain text
            r.note = cc.numberFor(r.u);
        }
        runs.push_back(std::move(r));
    }
    if (runs.empty()) return inlineHtml(text);
    // The walk below is O(spans × boundaries); cap it so a corrupt or
    // hostile block can't hang the GUI thread (whole render is
    // synchronous in a QML binding). Real docs are nowhere close.
    if (runs.size() > 2000) runs.resize(2000);

    std::set<int> bounds{0, len};
    for (const Run& r : runs) { bounds.insert(r.s); bounds.insert(r.e); }

    auto openTag = [](const Run& r) -> QString {
        if (r.k == QLatin1String("comment"))
            return QStringLiteral("<span class=\"cmt\">");
        if (r.k == QLatin1String("link"))
            return QStringLiteral("<a href=\"%1\">").arg(safeHrefAttr(r.u));
        // Colors go through QColor, not string interpolation — a raw
        // value could smuggle extra CSS declarations into the style
        // attribute ("red;background:url(...)"). Invalid → unstyled
        // span, keeping open/close tags balanced.
        if (r.k == QLatin1String("color")) {
            const QColor c(r.u);
            if (!c.isValid()) return QStringLiteral("<span>");
            return QStringLiteral("<span style=\"color:%1\">").arg(c.name());
        }
        if (r.k == QLatin1String("highlight")) {
            const QColor c(r.u);
            if (!c.isValid()) return QStringLiteral("<span>");
            return QStringLiteral("<span style=\"background:%1;color:%2;padding:1px 2px\">")
                .arg(c.name(), contrastOn(r.u));
        }
        if (r.k == QLatin1String("bold"))      return QStringLiteral("<strong>");
        if (r.k == QLatin1String("italic"))    return QStringLiteral("<em>");
        if (r.k == QLatin1String("strike"))    return QStringLiteral("<s>");
        if (r.k == QLatin1String("underline")) return QStringLiteral("<u>");
        if (r.k == QLatin1String("code"))      return QStringLiteral("<code>");
        return {};
    };
    auto closeTag = [&cc](const Run& r) -> QString {
        if (r.k == QLatin1String("comment")) {
            // Thread rides inside the tinted range as a hover card (spans
            // only — a div inside <p> would trip the HTML parser), plus a
            // superscript link into the trailing section.
            return QStringLiteral("<span class=\"cmtcard\">%1</span></span>"
                                  "<sup class=\"cref\"><a href=\"#c%2\">%2</a></sup>")
                .arg(cc.cardMsgs.value(r.u))
                .arg(r.note);
        }
        if (r.k == QLatin1String("link"))      return QStringLiteral("</a>");
        if (r.k == QLatin1String("color")
            || r.k == QLatin1String("highlight")) return QStringLiteral("</span>");
        if (r.k == QLatin1String("bold"))      return QStringLiteral("</strong>");
        if (r.k == QLatin1String("italic"))    return QStringLiteral("</em>");
        if (r.k == QLatin1String("strike"))    return QStringLiteral("</s>");
        if (r.k == QLatin1String("underline")) return QStringLiteral("</u>");
        if (r.k == QLatin1String("code"))      return QStringLiteral("</code>");
        return {};
    };

    QString out;
    std::vector<Run> stack;
    auto it = bounds.begin();
    int prev = *it;
    for (++it; it != bounds.end(); ++it) {
        const int a = prev, b = *it;
        prev = *it;
        if (a >= b) continue;
        std::vector<Run> desired;
        for (const Run& r : runs)
            if (r.s <= a && r.e >= b) desired.push_back(r);
        std::sort(desired.begin(), desired.end(), [](const Run& x, const Run& y) {
            if (x.rank != y.rank) return x.rank < y.rank;
            if (x.s != y.s) return x.s < y.s;
            return x.u < y.u;
        });
        size_t common = 0;
        while (common < stack.size() && common < desired.size()
               && stack[common] == desired[common]) ++common;
        while (stack.size() > common) { out += closeTag(stack.back()); stack.pop_back(); }
        for (size_t i = common; i < desired.size(); ++i) {
            out += openTag(desired[i]);
            stack.push_back(desired[i]);
        }
        out += escapeHtml(text.mid(a, b - a));
    }
    while (!stack.empty()) { out += closeTag(stack.back()); stack.pop_back(); }
    out.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    return out;
}

// Media descriptor "src" → absolute local path ("" when unresolvable,
// e.g. the portable {vol,rel} object form we don't map in v1).
QString resolveMediaSrc(const QJsonValue& src, const QString& docDir) {
    if (!src.isString()) return {};
    const QString s = src.toString();
    if (s.startsWith(QLatin1String(".minnotes/")))
        return docDir + QLatin1Char('/') + s;
    if (s.startsWith(QLatin1String("http")))
        return {};                        // remote — skip in preview
    return s;                             // absolute reference
}

// ── Stroke playback — ported from minNotes ─────────────────────────────
// (annotation_thumbnail.cpp paintStroke/strokeBoundsNorm + the
// annotation_serializer JSON schema). Strokes are stored post-smoothing
// (is_modeled), so playback is pure geometry: same pen, caps, arrowhead
// angle and oval/rect point conventions as the minNotes exporter, so the
// preview matches its HTML export.

struct Stroke {
    QString type;                 // freehand | line | rect | oval | arrow
    QColor color{255, 0, 0, 255};
    double width = 4.0;
    bool filled = false;
    QVector<QPointF> pts;
};

QVector<Stroke> parseShapes(const QJsonArray& shapes) {
    QVector<Stroke> out;
    out.reserve(shapes.size());
    for (const QJsonValue& v : shapes) {
        const QJsonObject o = v.toObject();
        Stroke s;
        s.type = o.value(QStringLiteral("type")).toString();
        if (s.type.isEmpty()) continue;
        const QJsonArray c = o.value(QStringLiteral("color")).toArray();
        if (c.size() >= 4)
            s.color = QColor::fromRgbF(float(c.at(0).toDouble()),
                                       float(c.at(1).toDouble()),
                                       float(c.at(2).toDouble()),
                                       float(c.at(3).toDouble()));
        s.width = o.value(QStringLiteral("stroke_width")).toDouble(4.0);
        s.filled = o.value(QStringLiteral("filled")).toBool(false);
        for (const QJsonValue& pv : o.value(QStringLiteral("points")).toArray()) {
            const QJsonArray pa = pv.toArray();
            if (pa.size() >= 2)
                s.pts.append(QPointF(pa.at(0).toDouble(), pa.at(1).toDouble()));
        }
        if (!s.pts.isEmpty()) out.append(std::move(s));
    }
    return out;
}

// Oval encodes {center, radii} — its true box is center ± radii; everything
// else is a bbox of actual coordinates.
QRectF strokeBounds(const Stroke& s) {
    if (s.pts.isEmpty()) return {};
    if (s.type == QLatin1String("oval") && s.pts.size() >= 2) {
        const QPointF c = s.pts[0], r = s.pts[1];
        return QRectF(c.x() - std::abs(r.x()), c.y() - std::abs(r.y()),
                      2.0 * std::abs(r.x()), 2.0 * std::abs(r.y()));
    }
    double minX = s.pts.front().x(), maxX = minX;
    double minY = s.pts.front().y(), maxY = minY;
    for (const QPointF& p : s.pts) {
        minX = std::min(minX, p.x()); maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y()); maxY = std::max(maxY, p.y());
    }
    return QRectF(minX, minY, maxX - minX, maxY - minY);
}

void paintStroke(QPainter& p, const Stroke& s, double w, double h,
                 double widthScale) {
    if (s.pts.isEmpty()) return;
    const double penW = std::max(1.0, s.width * widthScale);
    QPen pen(s.color);
    pen.setWidthF(penW);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    const QBrush fill(s.color);
    const auto px = [&](const QPointF& n) { return QPointF(n.x() * w, n.y() * h); };

    if (s.type == QLatin1String("freehand")) {
        if (s.pts.size() == 1) {
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawEllipse(px(s.pts[0]), penW * 0.5, penW * 0.5);
            return;
        }
        QPainterPath path;
        path.moveTo(px(s.pts[0]));
        for (int i = 1; i < s.pts.size(); ++i) path.lineTo(px(s.pts[i]));
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    } else if (s.type == QLatin1String("line")) {
        if (s.pts.size() < 2) return;
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(px(s.pts[0]), px(s.pts[1]));
    } else if (s.type == QLatin1String("rect")) {
        // points[0] / points[2] are opposite corners.
        if (s.pts.size() < 4) return;
        const QRectF rect = QRectF(px(s.pts[0]), px(s.pts[2])).normalized();
        if (s.filled) { p.setPen(Qt::NoPen); p.setBrush(fill); }
        else          { p.setPen(pen);      p.setBrush(Qt::NoBrush); }
        p.drawRect(rect);
    } else if (s.type == QLatin1String("oval")) {
        // points[0] = center; points[1] = radii as fractions of W / H.
        if (s.pts.size() < 2) return;
        const QPointF center = px(s.pts[0]);
        const double rx = s.pts[1].x() * w;
        const double ry = s.pts[1].y() * h;
        if (s.filled) { p.setPen(Qt::NoPen); p.setBrush(fill); }
        else          { p.setPen(pen);      p.setBrush(Qt::NoBrush); }
        p.drawEllipse(center, rx, ry);
    } else if (s.type == QLatin1String("arrow")) {
        if (s.pts.size() < 2) return;
        const QPointF start = px(s.pts[0]);
        const QPointF end = px(s.pts[1]);
        double dirX = end.x() - start.x();
        double dirY = end.y() - start.y();
        const double length = std::sqrt(dirX * dirX + dirY * dirY);
        if (length < 0.001) return;
        dirX /= length;
        dirY /= length;
        // 25° head; shaft stops arrowSize*cos short of the tip.
        const double arrowSize = 4.5 * widthScale + penW * 2.3;
        const double cosA = std::cos(0.436332);
        const double sinA = std::sin(0.436332);
        const QPointF lineEnd(end.x() - dirX * arrowSize * cosA,
                              end.y() - dirY * arrowSize * cosA);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(start, lineEnd);
        const QPointF a1(end.x() - arrowSize * (dirX * cosA - dirY * sinA),
                         end.y() - arrowSize * (dirY * cosA + dirX * sinA));
        const QPointF a2(end.x() - arrowSize * (dirX * cosA + dirY * sinA),
                         end.y() - arrowSize * (dirY * cosA - dirX * sinA));
        QPolygonF head;
        head << end << a1 << a2;
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPolygon(head);
    }
}

QString dataUri(const QImage& img) {
    if (img.isNull()) return {};
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return QStringLiteral("data:image/png;base64,")
        + QString::fromLatin1(bytes.toBase64());
}

// block_ink envelope: {version:"2.0", coordinate_system:"block-local",
// space:"px"|"frame", shapes:[…]}. Wrong version/system → empty (the
// minNotes reader rejects those too).
struct InkAnchor {
    bool frame = false;               // false = "px" (text anchor)
    QVector<Stroke> strokes;
};

bool parseInk(const QString& json, InkAnchor& out) {
    out.frame = false;
    out.strokes.clear();
    if (json.isEmpty()) return true;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) return false;
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toString() != QLatin1String("2.0"))
        return false;
    if (root.value(QStringLiteral("coordinate_system")).toString()
        != QLatin1String("block-local"))
        return false;
    out.frame = root.value(QStringLiteral("space")).toString()
        == QLatin1String("frame");
    out.strokes = parseShapes(root.value(QStringLiteral("shapes")).toArray());
    return true;
}

// Sketch content → 2x transparent raster (placed images first, then
// strokes; stroke_width is in source-canvas px, hence the W/w pen scale).
QImage renderSketch(const QJsonObject& o, const QString& docDir) {
    const int w = o.value(QStringLiteral("w")).toInt(480);
    const int h = o.value(QStringLiteral("h")).toInt(480);
    // Bounds-check BEFORE doubling — w near INT_MAX makes w * 2 signed
    // overflow. 4096 source px keeps the 2x canvas within 8192.
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return {};
    const int W = w * 2, H = h * 2;
    QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (const QJsonValue& v : o.value(QStringLiteral("images")).toArray()) {
        const QJsonObject im = v.toObject();
        const QString abs = resolveMediaSrc(im.value(QStringLiteral("src")), docDir);
        if (abs.isEmpty()) continue;
        const QImage src(abs);
        if (src.isNull()) continue;
        p.drawImage(QRectF(im.value(QStringLiteral("x")).toDouble() * W,
                           im.value(QStringLiteral("y")).toDouble() * H,
                           im.value(QStringLiteral("w")).toDouble() * W,
                           im.value(QStringLiteral("h")).toDouble() * H),
                    src);
    }
    for (const Stroke& s : parseShapes(o.value(QStringLiteral("shapes")).toArray()))
        paintStroke(p, s, W, H, double(W) / double(w));
    p.end();
    return img;
}

// Frame-space ink → transparent PNG at the media's intrinsic size (points
// are display-frame fractions; stroke_width is already intrinsic px).
QImage renderFrameInk(const InkAnchor& a, int mediaW, int mediaH) {
    if (!a.frame || a.strokes.isEmpty() || mediaW <= 0 || mediaH <= 0
        || mediaW > 8192 || mediaH > 8192)   // descriptor-controlled —
        return {};                           // clamp like the other rasters
    QImage img(mediaW, mediaH, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const Stroke& s : a.strokes)
        paintStroke(p, s, mediaW, mediaH, 1.0);
    p.end();
    return img;
}

// Px-space (text-block) ink → 2x bbox-cropped PNG + its page-px box.
// Point = (Δx from PAGE CENTER, Δy from block top); the preview column is
// the same 760 measure the ink was drawn against, so X is exact. The size
// guard drops corrupt blobs instead of exploding the canvas.
struct TextInk { QImage img; QRectF box; };
TextInk renderTextInk(const InkAnchor& a) {
    TextInk out;
    if (a.frame || a.strokes.isEmpty()) return out;
    QRectF box;
    for (const Stroke& st : a.strokes) {
        QRectF b = strokeBounds(st);
        const double pad = std::max(2.0, st.width);
        b.adjust(-pad, -pad, pad, pad);
        box = box.isNull() ? b : box.united(b);
    }
    // Range-check in double space: stroke coords are unclamped JSON
    // doubles, and double→int of an out-of-int-range value is UB (and
    // lands differently on arm64 vs x86). NaN fails the > 0 tests.
    const double Wd = std::ceil(box.width() * 2.0);
    const double Hd = std::ceil(box.height() * 2.0);
    if (!(Wd > 0.0) || !(Hd > 0.0) || Wd > 8192.0 || Hd > 8192.0) return out;
    const int W = int(Wd), H = int(Hd);
    QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(2.0, 2.0);
    p.translate(-box.left(), -box.top());
    for (const Stroke& st : a.strokes)
        paintStroke(p, st, 1.0, 1.0, 1.0);
    p.end();
    out.img = img;
    out.box = box;
    return out;
}

// Insert `tag` before the block element's final closing tag (the minNotes
// injectInk pattern) so absolutely-positioned children anchor to it.
QString insertBeforeClose(QString blk, const QString& tag) {
    const int at = blk.lastIndexOf(QStringLiteral("</"));
    if (at < 0) return blk + tag;
    blk.insert(at, tag);
    return blk;
}

QString mediaHtml(const QString& content, const QString& docDir,
                  const InkAnchor& ink) {
    const QJsonObject o = QJsonDocument::fromJson(content.toUtf8()).object();
    const QString kind = o.value(QStringLiteral("kind")).toString();

    // Illustrations: strokes live inline in the content JSON — there is no
    // file on disk to point at. Raster them like the minNotes exporter.
    if (kind == QLatin1String("sketch")) {
        const QString src = dataUri(renderSketch(o, docDir));
        if (!src.isEmpty())
            return QStringLiteral(
                "<figure><img class=\"sketch\" src=\"%1\" alt=\"Sketch\"></figure>")
                .arg(src);
        return QStringLiteral("<div class=\"chip\">&#9998; (empty sketch)</div>");
    }

    const QString abs = resolveMediaSrc(o.value(QStringLiteral("src")), docDir);
    const QString name = escapeHtml(QFileInfo(abs).fileName());

    const bool isImage = kind == QLatin1String("image")
        || (kind.isEmpty() && !abs.isEmpty());  // old image blocks may omit kind
    if (isImage && !abs.isEmpty() && QFileInfo::exists(abs)) {
        // Honour the user-set display width (dw); untouched images stay
        // responsive via max-width:100%.
        const int dw = o.value(QStringLiteral("dw")).toInt(0);
        const QString wstyle = dw > 0
            ? QStringLiteral(" style=\"width:%1px;max-width:none\"").arg(dw)
            : QString();
        const QString imgUrl =
            QUrl::fromLocalFile(abs).toString(QUrl::FullyEncoded);
        // Frame-space margin ink overlays the image exactly (z-stack), at
        // the media's intrinsic size from the descriptor.
        const QString inkSrc = dataUri(renderFrameInk(
            ink, o.value(QStringLiteral("w")).toInt(0),
            o.value(QStringLiteral("h")).toInt(0)));
        if (!inkSrc.isEmpty())
            return QStringLiteral(
                "<figure><div class=\"inkwrap\"%1><img src=\"%2\" alt=\"%3\"%4>"
                "<img class=\"ink\" src=\"%5\" alt=\"\"></div></figure>")
                .arg(wstyle, imgUrl, name,
                     dw > 0 ? QStringLiteral(" style=\"width:100%\"") : QString(),
                     inkSrc);
        return QStringLiteral("<figure><img src=\"%1\" alt=\"%2\"%3></figure>")
            .arg(imgUrl, name, wstyle);
    }
    // Video / PDF / file / unresolvable → an honest chip, not a broken
    // image. (No poster extraction in the preview, so frame ink on these
    // has nothing to overlay and is dropped.)
    const QString glyph = kind == QLatin1String("video") ? QStringLiteral("&#9654;")
                        : kind == QLatin1String("pdf")   ? QStringLiteral("&#128196;")
                        : kind == QLatin1String("file")  ? QStringLiteral("&#128206;")
                                                         : QStringLiteral("&#128279;");
    QString label = name;
    if (label.isEmpty())
        label = escapeHtml(o.value(QStringLiteral("name")).toString());
    if (label.isEmpty()) label = QStringLiteral("(unavailable media)");
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

QString commentStamp(qint64 t) {
    if (t <= 0) return {};
    // Tolerate both s and ms epochs.
    const QDateTime dt = t > 100000000000LL
        ? QDateTime::fromMSecsSinceEpoch(t)
        : QDateTime::fromSecsSinceEpoch(t);
    return dt.toString(QStringLiteral("yyyy-MM-dd hh:mm"));
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
    ".title{font-size:26px;font-weight:700;color:#f0f0f0;margin-bottom:24px}"
    // Ink stack (mirrors the minNotes export CSS): frame ink rides its
    // media; px ink is absolutely positioned inside its host block.
    "img.ink{pointer-events:none}"
    ".inkwrap{position:relative}"
    ".inkwrap img{display:block;max-width:100%}"
    ".inkwrap .ink{position:absolute;inset:0;width:100%;z-index:1;background:transparent}"
    ".inkhost{position:relative}"
    ".cref{font-size:.72em;color:#0189f1;vertical-align:super}"
    // Comment ranges: tinted anchor + hover thread card (spans styled as
    // blocks — a real <p> inside a span would trip the parser).
    ".cmt{background:rgba(1,137,241,.13);position:relative}"
    ".cmt .cmtcard{display:none;position:absolute;left:0;top:1.6em;z-index:30;"
    "width:300px;background:#202020;border:1px solid #2a2a2a;padding:10px 12px;"
    "font-size:13px;font-style:normal;font-weight:400;line-height:1.5;color:#e4e3e2}"
    ".cmt:hover .cmtcard{display:block}"
    ".cmtcard .cmsg{display:block;margin-bottom:6px}"
    ".comments{margin-top:32px;border-top:1px solid #333;padding-top:12px}"
    ".comments h3{color:#f0f0f0;font-size:15px}"
    ".cthread{margin:10px 0}"
    ".cmsg{margin:2px 0 2px 18px}"
    ".stamp{color:#8a8a8a;font-size:11px;margin-left:8px}"
    ".resolved{color:#8a8a8a;font-style:italic}";

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
    QString commentsHtml;
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

            // Margin ink, one JSON blob per anchored block (schema v2; the
            // table is absent in v1 docs — a failed exec just means none).
            QHash<QString, QString> inkByBlock;
            QSqlQuery inkQ(db);
            if (inkQ.exec(QStringLiteral("SELECT block_id, ink FROM block_ink")))
                while (inkQ.next())
                    inkByBlock.insert(inkQ.value(0).toString(),
                                      inkQ.value(1).toString());

            // Comment threads + messages (also v2-only). Numbered in block
            // order at anchor time; orphaned threads simply never surface.
            // Messages are pre-built twice: div-wrapped for the trailing
            // section, span-wrapped for the in-text hover cards (a div
            // inside <p> would trip the HTML parser).
            CommentCtx cc;
            QSqlQuery thQ(db);
            if (thQ.exec(QStringLiteral("SELECT id, resolved FROM comment_threads")))
                while (thQ.next())
                    cc.resolved.insert(thQ.value(0).toString(),
                                       thQ.value(1).toInt() != 0);
            QSqlQuery msgQ(db);
            if (msgQ.exec(QStringLiteral(
                    "SELECT thread_id, body, created FROM comment_messages "
                    "ORDER BY created")))
                while (msgQ.next()) {
                    const QString tid = msgQ.value(0).toString();
                    const QString stamp = commentStamp(msgQ.value(2).toLongLong());
                    QString body = escapeHtml(msgQ.value(1).toString());
                    body.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
                    const QString stampHtml = stamp.isEmpty()
                        ? QString()
                        : QStringLiteral("<span class=\"stamp\">%1</span>").arg(stamp);
                    cc.sectionMsgs[tid] += QStringLiteral("<div class=\"cmsg\">%1%2</div>")
                        .arg(body, stampHtml);
                    cc.cardMsgs[tid] += QStringLiteral("<span class=\"cmsg\">%1%2</span>")
                        .arg(body, stampHtml);
                }

            QSqlQuery q(db);
            if (q.exec(QStringLiteral(
                    "SELECT id, type, attrs, content, depth FROM blocks ORDER BY rank"))) {
                ok = true;
                int orderedN = 0;
                while (q.next()) {
                    const QString blockId = q.value(0).toString();
                    const QString type = q.value(1).toString();
                    const QJsonObject attrs = QJsonDocument::fromJson(
                        q.value(2).toString().toUtf8()).object();
                    const QString content = q.value(3).toString();
                    const int depth = q.value(4).toInt();
                    const bool ordered = type == QLatin1String("ordered_item");
                    if (!ordered) orderedN = 0;

                    InkAnchor ink;
                    parseInk(inkByBlock.value(blockId), ink);

                    // Span formatting when the block carries spans (v1+
                    // clean-text convention); markdown-ish fallback keeps
                    // literal-markdown docs readable.
                    const QJsonArray spans =
                        attrs.value(QStringLiteral("spans")).toArray();
                    const auto inl = [&](const QString& t) {
                        return spans.isEmpty() ? inlineHtml(t)
                                               : spansHtml(t, spans, cc);
                    };

                    double indent = 0.0;    // block's own left offset in page px
                    QString blk;
                    if (type == QLatin1String("heading")) {
                        const int lv = qBound(1, attrs.value(QStringLiteral("level")).toInt(1), 6);
                        blk = QStringLiteral("<h%1>%2</h%1>").arg(lv).arg(inl(content));
                    } else if (type == QLatin1String("quote")) {
                        blk = QStringLiteral("<blockquote>%1</blockquote>").arg(inl(content));
                    } else if (type == QLatin1String("code")) {
                        blk = QStringLiteral("<pre><code>%1</code></pre>").arg(escapeHtml(content));
                    } else if (type == QLatin1String("divider")) {
                        blk = QStringLiteral("<hr>");
                    } else if (type == QLatin1String("table")) {
                        blk = tableHtml(content);
                    } else if (type == QLatin1String("media")) {
                        blk = mediaHtml(content, docDir, ink);
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
                        indent = qBound(0, depth, 8) * 22;
                        blk = QStringLiteral(
                            "<div class=\"li\" style=\"margin-left:%1px\">%2 %3</div>")
                            .arg(indent).arg(bullet, inl(content));
                    } else {  // paragraph + unknown future types degrade to text
                        blk = content.isEmpty()
                            ? QStringLiteral("<p>&nbsp;</p>")
                            : QStringLiteral("<p>%1</p>").arg(inl(content));
                    }
                    // (Comment anchors are emitted by the span walk itself:
                    // tinted range + hover card + superscript link.)

                    // Text-anchored margin ink: absolutely positioned inside a
                    // relative host, X from the page center (380 in the 760
                    // measure) minus the block's own indent.
                    const TextInk ti = renderTextInk(ink);
                    if (!ti.img.isNull()) {
                        const QString tag = QStringLiteral(
                            "<img class=\"ink\" style=\"position:absolute;left:%1px;"
                            "top:%2px;width:%3px;height:%4px;z-index:2\" src=\"%5\" alt=\"\">")
                            .arg(380.0 + ti.box.left() - indent)
                            .arg(ti.box.top())
                            .arg(ti.box.width())
                            .arg(ti.box.height())
                            .arg(dataUri(ti.img));
                        blk = QStringLiteral("<div class=\"inkhost\">%1</div>")
                            .arg(insertBeforeClose(blk, tag));
                    }

                    body += blk;
                }
            }

            if (!cc.order.isEmpty()) {
                commentsHtml = QStringLiteral(
                    "<section class=\"comments\"><h3>Comments</h3>");
                for (const QString& tid : cc.order) {
                    commentsHtml += QStringLiteral(
                        "<div class=\"cthread\" id=\"c%1\"><b>%1.</b>%2%3</div>")
                        .arg(cc.num.value(tid))
                        .arg(cc.resolved.value(tid)
                                 ? QStringLiteral(" <span class=\"resolved\">(resolved)</span>")
                                 : QString(),
                             cc.sectionMsgs.value(tid,
                                 QStringLiteral("<div class=\"cmsg\">(no messages)</div>")));
                }
                commentsHtml += QStringLiteral("</section>");
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
    html += body + commentsHtml + QStringLiteral("</main>");

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

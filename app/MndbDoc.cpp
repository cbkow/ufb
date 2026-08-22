#include "MndbDoc.h"

#include "miniz.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPolygonF>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTextDocument>
#include <QTextOption>
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
// approximation for span-less (pre-v1) blocks only; minNotes itself
// never stores markers any more (they are consumed into spans on load).
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

// ── Colors ─────────────────────────────────────────────────────────────
// Every color below comes from doc data and goes through QColor, never
// string interpolation — a raw value could smuggle extra CSS
// declarations into a style attribute ("red;background:url(...)").

// Highlight text needs contrast against the user's swatch, chosen by luma.
QString contrastOn(const QString& hex) {
    const QColor c(hex);
    if (!c.isValid()) return QStringLiteral("#f0f0f0");
    const double luma = 0.299 * c.red() + 0.587 * c.green() + 0.114 * c.blue();
    return luma > 140.0 ? QStringLiteral("#111111") : QStringLiteral("#f0f0f0");
}

QString rgba(const QColor& c, double a) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(a);
}

// Choice chip = the app's pill recipe: option color at 0.28 alpha fill +
// 0.55 alpha border, neutral ground when colorless; squared corners.
QString chipOpen(const QString& colorHex) {
    const QColor c(colorHex);
    if (!c.isValid())
        return QStringLiteral("<span class=\"chip\">");
    return QStringLiteral("<span class=\"chip\" style=\"background:%1;border-color:%2\">")
        .arg(rgba(c, 0.28), rgba(c, 0.55));
}

// Choice payload {"o":[{id,l,c}],"v":id} (JSON-in-string in the span's u
// and in the table column-type map). Returns the selected option's color
// ("" when v matches nothing / color omitted) and its label.
struct ChoicePick { QString label, color; bool found = false; };
ChoicePick pickChoice(const QJsonArray& options, const QString& selected) {
    ChoicePick out;
    for (const QJsonValue& v : options) {
        const QJsonObject opt = v.toObject();
        if (opt.value(QStringLiteral("id")).toString() == selected) {
            out.label = opt.value(QStringLiteral("l")).toString();
            out.color = opt.value(QStringLiteral("c")).toString();
            out.found = true;
            break;
        }
    }
    return out;
}

QString choiceSpanColor(const QString& u) {
    const QJsonObject o = QJsonDocument::fromJson(u.toUtf8()).object();
    return pickChoice(o.value(QStringLiteral("o")).toArray(),
                      o.value(QStringLiteral("v")).toString()).color;
}

// ── Span-based inline formatting — ported from minNotes ────────────────
// (Exporter.cpp emitInlineHtml). Docs store CLEAN text plus spans
// [{s,e,k,u}]; block spans carry STRING kinds, table-cell spans carry the
// raw INTEGER enum (1..10) — both normalise to the string names here. The
// walk splits the text at every span boundary and keeps a tag stack
// ordered by rank, so overlapping spans nest deterministically.

QString spanKindName(const QJsonValue& k) {
    if (k.isString()) return k.toString();
    switch (k.toInt(-1)) {
    case 1:  return QStringLiteral("bold");
    case 2:  return QStringLiteral("italic");
    case 3:  return QStringLiteral("code");
    case 4:  return QStringLiteral("strike");
    case 5:  return QStringLiteral("underline");
    case 6:  return QStringLiteral("link");
    case 7:  return QStringLiteral("color");
    case 8:  return QStringLiteral("highlight");
    case 9:  return QStringLiteral("comment");
    case 10: return QStringLiteral("choice");
    default: return {};
    }
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
    if (k == QLatin1String("choice"))    return 9;   // innermost, atomic
    return 10;                                       // unknown → dropped
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
        int s, e, rank; QString k, u; int note = 0; bool resolved = false;
        bool operator==(const Run& o) const {
            return k == o.k && u == o.u && s == o.s && e == o.e;
        }
    };
    std::vector<Run> runs;
    for (const QJsonValue& v : spans) {
        const QJsonObject sp = v.toObject();
        const QString k = spanKindName(sp.value(QStringLiteral("k")));
        const int rank = spanRank(k);
        if (rank > 9) continue;
        const int s = std::clamp(sp.value(QStringLiteral("s")).toInt(), 0, len);
        const int e = std::clamp(sp.value(QStringLiteral("e")).toInt(), 0, len);
        if (s >= e) continue;
        Run r{s, e, rank, k, sp.value(QStringLiteral("u")).toString(), 0, false};
        if (k == QLatin1String("comment")) {
            if (!cc.known(r.u)) continue;   // orphaned anchor → plain text
            r.note = cc.numberFor(r.u);
            r.resolved = cc.resolved.value(r.u, false);
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
            return r.resolved ? QStringLiteral("<span class=\"cmt resolved\">")
                              : QStringLiteral("<span class=\"cmt\">");
        if (r.k == QLatin1String("link"))
            return QStringLiteral("<a href=\"%1\">").arg(safeHrefAttr(r.u));
        // Invalid color → unstyled span, keeping open/close tags balanced.
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
        if (r.k == QLatin1String("choice"))    return chipOpen(choiceSpanColor(r.u));
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
            || r.k == QLatin1String("highlight")
            || r.k == QLatin1String("choice")) return QStringLiteral("</span>");
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

// Span formatting when the block carries spans (clean-text convention);
// markdown-ish fallback keeps literal-markdown (pre-v1) docs readable.
QString richText(const QString& text, const QJsonArray& spans, CommentCtx& cc) {
    return spans.isEmpty() ? inlineHtml(text) : spansHtml(text, spans, cc);
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

// Human-readable reference for a src we may not be able to open (shown in
// the reference figure's path line): strings as-is, {vol,rel} as the
// joined segments.
QString describeSrc(const QJsonValue& src) {
    if (src.isString()) return src.toString();
    if (src.isObject()) {
        const QJsonObject o = src.toObject();
        QStringList segs;
        for (const QJsonValue& v : o.value(QStringLiteral("rel")).toArray())
            segs << v.toString();
        return QStringLiteral("{%1}/%2")
            .arg(o.value(QStringLiteral("vol")).toString(), segs.join(QLatin1Char('/')));
    }
    return {};
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

// ── Text chips — ported from minNotes sketch_text.cpp ──────────────────
// A text element {text,x,y,w,size,color} renders as a filled, squared
// chip: `color` is the fill, glyphs auto-pick black/white by luma, padding
// = 0.4·size, height is DERIVED from wrapping (never stored). Layout
// happens in SOURCE space (font at `size` px, width = w·srcW) and the
// painter scales, so wrap points match the app at any raster scale.
// Units follow the container: sketch / frame ink → x,y,w normalized to
// the frame, size in source px; px ink → everything in page px.

struct TextChip {
    QString text;
    double x = 0, y = 0, w = 0, size = 16;
    QColor color{0xE4, 0xE3, 0xE2};
};

std::vector<TextChip> parseTextChips(const QJsonObject& root) {
    std::vector<TextChip> out;
    const QJsonArray arr = root.value(QStringLiteral("texts")).toArray();
    // Caps: each chip lays out a QTextDocument on the GUI thread.
    const int n = std::min<int>(arr.size(), 200);
    out.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        const QJsonObject o = arr.at(i).toObject();
        TextChip t;
        t.text = o.value(QStringLiteral("text")).toString().left(4000);
        t.x    = o.value(QStringLiteral("x")).toDouble();
        t.y    = o.value(QStringLiteral("y")).toDouble();
        t.w    = o.value(QStringLiteral("w")).toDouble();
        t.size = std::clamp(o.value(QStringLiteral("size")).toDouble(16), 1.0, 512.0);
        const QColor c(o.value(QStringLiteral("color")).toString(QStringLiteral("#E4E3E2")));
        if (c.isValid()) t.color = c;
        if (!std::isfinite(t.x) || !std::isfinite(t.y) || !std::isfinite(t.w)) continue;
        if (t.text.isEmpty() || t.w <= 0) continue;
        out.push_back(std::move(t));
    }
    return out;
}

// The app font for chips: minNotes bundles Aspekta (we don't) → Inter →
// system UI font. Family only affects wrap points, not geometry rules.
QString chipFontFamily() {
    static const QString family = [] {
        const QStringList all = QFontDatabase::families();
        for (const char* want : { "Aspekta 400", "Aspekta", "Inter 18pt", "Inter" })
            if (all.contains(QLatin1String(want)))
                return QString::fromLatin1(want);
        return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    }();
    return family;
}

double chipPad(const TextChip& t) { return t.size * 0.4; }

void configureChipDoc(QTextDocument& doc, const TextChip& t, double srcW) {
    QFont f(chipFontFamily());
    f.setPixelSize(std::max(1, int(std::lround(t.size))));
    doc.setDefaultFont(f);
    QTextOption opt;
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    doc.setDefaultTextOption(opt);
    doc.setDocumentMargin(0);
    doc.setPlainText(t.text);
    doc.setTextWidth(std::max(1.0, t.w * srcW - 2.0 * chipPad(t)));
}

// The element's full rect in SOURCE px (position + derived height).
QRectF chipRectSrc(const TextChip& t, double srcW, double srcH) {
    QTextDocument doc;
    configureChipDoc(doc, t, srcW);
    return QRectF(t.x * srcW, t.y * srcH, t.w * srcW,
                  doc.size().height() + 2.0 * chipPad(t));
}

// Paint at `scale` (raster px per source px).
void paintTextChip(QPainter& p, const TextChip& t,
                   double srcW, double srcH, double scale) {
    if (srcW <= 0 || scale <= 0) return;
    QTextDocument doc;
    configureChipDoc(doc, t, srcW);
    const double pad = chipPad(t);
    const double boxH = doc.size().height() + 2.0 * pad;
    const double luma = 0.299 * t.color.redF() + 0.587 * t.color.greenF()
                      + 0.114 * t.color.blueF();
    p.save();
    p.translate(t.x * srcW * scale, t.y * srcH * scale);
    p.scale(scale, scale);
    p.fillRect(QRectF(0, 0, t.w * srcW, boxH), t.color);   // the chip (squared)
    p.translate(pad, pad);
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text, luma > 0.55 ? QColor(0, 0, 0)
                                                     : QColor(255, 255, 255));
    doc.documentLayout()->draw(&p, ctx);
    p.restore();
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
// space:"px"|"frame", shapes:[…], texts:[…]}. Wrong version/system →
// empty (the minNotes reader rejects those too).
struct InkAnchor {
    bool frame = false;               // false = "px" (text anchor)
    QVector<Stroke> strokes;
    std::vector<TextChip> texts;
    bool empty() const { return strokes.isEmpty() && texts.empty(); }
};

bool parseInk(const QString& json, InkAnchor& out) {
    out.frame = false;
    out.strokes.clear();
    out.texts.clear();
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
    out.texts = parseTextChips(root);
    return true;
}

// Sketch content → transparent raster (placed images, then text chips,
// then strokes — the app's z-order, so ink can circle labels). 2× until
// the long edge would hit 8192, then scale down: a max-size canvas
// (8192, the app's cap) rasters at 1× instead of allocating a 1 GiB image.
QImage renderSketch(const QJsonObject& o, const QString& docDir) {
    const int w = o.value(QStringLiteral("w")).toInt(480);
    const int h = o.value(QStringLiteral("h")).toInt(480);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return {};
    const double s = std::min(2.0, 8192.0 / double(std::max(w, h)));
    const int W = int(std::lround(w * s)), H = int(std::lround(h * s));
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
    for (const TextChip& t : parseTextChips(o))
        paintTextChip(p, t, w, h, double(W) / double(w));
    for (const Stroke& s : parseShapes(o.value(QStringLiteral("shapes")).toArray()))
        paintStroke(p, s, W, H, double(W) / double(w));
    p.end();
    return img;
}

// Frame-space ink → transparent PNG at the media's intrinsic size. Points
// are display-frame fractions (values outside [0,1] = margin overshoot),
// stroke_width is intrinsic px. The raster covers frame ∪ ink bbox and
// `boxNorm` says where it sits in FRAME units — the HTML layer positions
// it with percent offsets (scales with the responsive image); a unit rect
// means exactly-the-frame (inset:0).
struct FrameInk { QImage img; QRectF boxNorm; };
FrameInk renderFrameInk(const InkAnchor& a, int mediaW, int mediaH) {
    FrameInk out;
    if (!a.frame || a.empty() || mediaW <= 0 || mediaH <= 0
        || mediaW > 8192 || mediaH > 8192)   // descriptor-controlled —
        return out;                          // clamp like the other rasters
    const double mw = mediaW, mh = mediaH;
    QRectF box(0, 0, 1, 1);
    for (const Stroke& s : a.strokes) {
        const double px = s.width / (2.0 * mw);
        const double py = s.width / (2.0 * mh);
        box = box.united(strokeBounds(s).adjusted(-px, -py, px, py));
    }
    for (const TextChip& t : a.texts) {
        const QRectF r = chipRectSrc(t, mw, mh);   // intrinsic px
        box = box.united(QRectF(r.x() / mw, r.y() / mh, r.width() / mw, r.height() / mh));
    }
    // Range-check in double space: coords are unclamped JSON doubles and
    // double→int of an out-of-range value is UB. NaN fails the > 0 tests.
    const double Wd = std::ceil(box.width() * mw);
    const double Hd = std::ceil(box.height() * mh);
    if (!(Wd > 0.0) || !(Hd > 0.0) || Wd > 8192.0 || Hd > 8192.0) return out;
    QImage img(int(Wd), int(Hd), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(-box.left() * mw, -box.top() * mh);
    for (const TextChip& t : a.texts)          // chips under the ink
        paintTextChip(p, t, mw, mh, 1.0);
    for (const Stroke& s : a.strokes)
        paintStroke(p, s, mw, mh, 1.0);
    p.end();
    out.img = img;
    out.boxNorm = box;
    return out;
}

// Inline geometry for an overshooting frame-ink layer: percent-of-frame
// offsets override the stylesheet's inset:0;width:100%. Unit rect → none.
QString frameInkStyle(const QRectF& b) {
    if (b == QRectF(0, 0, 1, 1)) return {};
    return QStringLiteral(" style=\"left:%1%;top:%2%;width:%3%;height:%4%;"
                          "right:auto;bottom:auto;max-width:none\"")
        .arg(b.left() * 100).arg(b.top() * 100)
        .arg(b.width() * 100).arg(b.height() * 100);
}

// Px-space (text-block) ink → 2x bbox-cropped PNG + its page-px box.
// Point = (Δx from PAGE CENTER, Δy from block top); the preview column is
// the same measure the ink was drawn against, so X is exact. The size
// guard drops corrupt blobs instead of exploding the canvas.
struct TextInk { QImage img; QRectF box; };
TextInk renderTextInk(const InkAnchor& a) {
    TextInk out;
    if (a.frame || a.empty()) return out;
    QRectF box;
    for (const Stroke& st : a.strokes) {
        QRectF b = strokeBounds(st);
        const double pad = std::max(2.0, st.width);
        b.adjust(-pad, -pad, pad, pad);
        box = box.isNull() ? b : box.united(b);
    }
    for (const TextChip& t : a.texts) {
        // px space: local units ARE page px (x from page center — negative ok).
        const QRectF b = chipRectSrc(t, 1.0, 1.0).adjusted(-2, -2, 2, 2);
        box = box.isNull() ? b : box.united(b);
    }
    // Range-check in double space (see renderFrameInk).
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
    for (const TextChip& t : a.texts)          // chips under the ink
        paintTextChip(p, t, 1.0, 1.0, 1.0);    // painter carries the 2×
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

QString humanSize(qint64 b) {
    if (b < 0) return {};
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(b);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return u == 0 ? QStringLiteral("%1 B").arg(b)
                  : QStringLiteral("%1 %2").arg(v, 0, 'f', 1).arg(QLatin1String(units[u]));
}

QString humanDuration(qint64 ms) {
    if (ms <= 0) return {};
    const qint64 s = ms / 1000;
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

// Non-image media (video / PDF / file / unresolvable) → the minNotes
// reference figure: name, path and a kind·meta line. No poster extraction
// in the preview, so frame ink on these has nothing to overlay.
QString referenceFigure(const QJsonObject& o, const QString& kind,
                        const QString& abs) {
    QString name = QFileInfo(abs).fileName();
    if (name.isEmpty()) name = o.value(QStringLiteral("name")).toString();
    if (name.isEmpty()) name = QFileInfo(describeSrc(o.value(QStringLiteral("src")))).fileName();
    if (name.isEmpty()) name = QStringLiteral("(unavailable media)");
    // Path line: the stored reference (".minnotes/…" doc-relative, the
    // {vol,rel} form, or a URL) reads better than a resolved temp path —
    // only machine-absolute srcs show as the absolute path.
    const QJsonValue srcV = o.value(QStringLiteral("src"));
    const QString path = (srcV.isString()
                          && srcV.toString().startsWith(QLatin1String(".minnotes/")))
        ? srcV.toString()
        : (abs.isEmpty() ? describeSrc(srcV) : abs);

    QStringList meta;
    const int w = o.value(QStringLiteral("w")).toInt(0);
    const int h = o.value(QStringLiteral("h")).toInt(0);
    if (kind == QLatin1String("video")) {
        if (w > 0 && h > 0) meta << QStringLiteral("%1×%2").arg(w).arg(h);
        const double fps = o.value(QStringLiteral("fps")).toDouble(0);
        if (fps > 0) meta << QStringLiteral("%1 fps").arg(fps, 0, 'g', 4);
        const QString dur = humanDuration(qint64(o.value(QStringLiteral("durMs")).toDouble(0)));
        if (!dur.isEmpty()) meta << dur;
        const int frames = o.value(QStringLiteral("frames")).toInt(0);
        if (frames > 0) meta << QStringLiteral("%1 frames").arg(frames);
    } else if (kind == QLatin1String("pdf")) {
        const int pages = o.value(QStringLiteral("pages")).toInt(0);
        if (pages > 0) meta << QStringLiteral("%1 pages").arg(pages);
    } else {
        const QFileInfo fi(abs);
        meta << (!abs.isEmpty() && fi.exists() ? humanSize(fi.size())
                                               : QStringLiteral("(unavailable)"));
    }
    const QString kindLabel = kind.isEmpty() ? QStringLiteral("image") : kind;
    return QStringLiteral(
        "<figure class=\"ref\"><figcaption><div class=\"fname\">%1</div>"
        "<div class=\"fpath\">%2</div><div class=\"fmeta\">%3</div></figcaption></figure>")
        .arg(escapeHtml(name), escapeHtml(path),
             escapeHtml(meta.isEmpty() ? kindLabel
                                       : kindLabel + QStringLiteral(" · ") + meta.join(QStringLiteral(" · "))));
}

QString mediaHtml(const QString& content, const QString& docDir,
                  const InkAnchor& ink, int pageWidth) {
    const QJsonObject o = QJsonDocument::fromJson(content.toUtf8()).object();
    const QString kind = o.value(QStringLiteral("kind")).toString();

    // Illustrations: strokes live inline in the content JSON — there is no
    // file on disk to point at. Raster them like the minNotes exporter.
    // Display width = user dw, else the page measure (sketches fill the
    // page since minNotes 0.4.2); never wider than the preview column.
    if (kind == QLatin1String("sketch")) {
        const QString src = dataUri(renderSketch(o, docDir));
        if (!src.isEmpty()) {
            const int dw = o.value(QStringLiteral("dw")).toInt(0);
            const int shown = std::clamp(dw > 0 ? dw : pageWidth, 1, 65535);
            return QStringLiteral(
                "<figure><img class=\"sketch\" src=\"%1\" alt=\"Sketch\" "
                "style=\"width:%2px\"></figure>")
                .arg(src).arg(shown);
        }
        return QStringLiteral("<div class=\"mchip\">&#9998; (empty sketch)</div>");
    }

    const QString abs = resolveMediaSrc(o.value(QStringLiteral("src")), docDir);
    const QString name = escapeHtml(QFileInfo(abs).fileName());

    const bool isImage = kind == QLatin1String("image")
        || (kind.isEmpty() && !abs.isEmpty());  // image blocks omit kind
    if (isImage && !abs.isEmpty() && QFileInfo::exists(abs)) {
        // Honour the user-set display width (dw); untouched images stay
        // responsive via max-width:100%.
        const int dw = o.value(QStringLiteral("dw")).toInt(0);
        const QString wstyle = dw > 0
            ? QStringLiteral(" style=\"width:%1px;max-width:none\"").arg(std::min(dw, 65535))
            : QString();
        const QString imgUrl =
            QUrl::fromLocalFile(abs).toString(QUrl::FullyEncoded);
        // Frame-space margin ink overlays the image (z-stack), rendered at
        // the media's intrinsic size from the descriptor and positioned in
        // frame percentages so it scales with the responsive image. The
        // wrapper takes the app's display width (dw, else min(page,
        // intrinsic)) so the overlay sizes to the IMAGE, not the column.
        const int iw = o.value(QStringLiteral("w")).toInt(0);
        const FrameInk fi = renderFrameInk(ink, iw, o.value(QStringLiteral("h")).toInt(0));
        if (!fi.img.isNull()) {
            const QString wrapStyle = dw > 0
                ? QStringLiteral(" style=\"width:%1px;max-width:none\"").arg(std::min(dw, 65535))
                : QStringLiteral(" style=\"width:%1px;max-width:100%\"").arg(std::min(pageWidth, iw));
            return QStringLiteral(
                "<figure><div class=\"inkwrap\"%1><img src=\"%2\" alt=\"%3\" style=\"width:100%\">"
                "<img class=\"ink\" src=\"%4\" alt=\"\"%5></div></figure>")
                .arg(wrapStyle, imgUrl, name, dataUri(fi.img), frameInkStyle(fi.boxNorm));
        }
        return QStringLiteral("<figure><img src=\"%1\" alt=\"%2\"%3></figure>")
            .arg(imgUrl, name, wstyle);
    }
    return referenceFigure(o, kind, abs);
}

// ── Tables ─────────────────────────────────────────────────────────────
// Grid JSON (TableGrid::toJson): {cols:N, header:H, w:[px|0], a:[0|1|2],
// rbg/rfg/cbg/cfg:[hex|""], ct:{"<col>":{k:1|2,o:[{id,l,c}]}},
// rows:[[cell…]]} where a cell is a bare string or {t,bg,fg,s,m,v}:
// s = spans (INTEGER kinds), m = media descriptor JSON-in-string,
// v = choice option id (choice column) | "1"/"2" check state (check
// column; 0 is stored as absent). Colors cascade cell → row → column.

QString taskGlyph(int state) {
    if (state == 1) return QStringLiteral("<span class=\"cb doing\"></span>");
    if (state == 2) return QStringLiteral("<span class=\"cb done\"></span>");
    return QStringLiteral("<span class=\"cb\"></span>");
}

QString tableHtml(const QString& gridJson, const QString& docDir,
                  CommentCtx& cc, int pageWidth) {
    const QJsonObject o = QJsonDocument::fromJson(gridJson.toUtf8()).object();
    const QJsonArray rows = o.value(QStringLiteral("rows")).toArray();
    int cols = o.value(QStringLiteral("cols")).toInt(0);
    for (const QJsonValue& r : rows) cols = std::max<int>(cols, r.toArray().size());
    cols = std::clamp(cols, 1, 512);
    const int header = std::clamp(o.value(QStringLiteral("header")).toInt(1), 0, int(rows.size()));

    const QJsonArray wArr = o.value(QStringLiteral("w")).toArray();
    const QJsonArray aArr = o.value(QStringLiteral("a")).toArray();
    const QJsonArray rbg = o.value(QStringLiteral("rbg")).toArray();
    const QJsonArray rfg = o.value(QStringLiteral("rfg")).toArray();
    const QJsonArray cbg = o.value(QStringLiteral("cbg")).toArray();
    const QJsonArray cfg = o.value(QStringLiteral("cfg")).toArray();
    const QJsonObject ct = o.value(QStringLiteral("ct")).toObject();

    struct ColType { int kind = 0; QJsonArray options; };
    std::vector<ColType> types(static_cast<size_t>(cols));
    for (auto it = ct.begin(); it != ct.end(); ++it) {
        bool okIdx = false;
        const int c = it.key().toInt(&okIdx);
        if (!okIdx || c < 0 || c >= cols) continue;
        if (it.value().isArray()) {            // legacy: bare option array = choice
            types[size_t(c)] = {1, it.value().toArray()};
        } else {
            const QJsonObject ce = it.value().toObject();
            const int k = ce.value(QStringLiteral("k")).toInt(1);
            types[size_t(c)] = {k == 2 ? 2 : 1, ce.value(QStringLiteral("o")).toArray()};
        }
    }
    auto colW = [&](int c) { return c < wArr.size() ? std::max(0, wArr.at(c).toInt(0)) : 0; };

    // Trailing fully-empty PLAIN columns drop (typed columns are structure).
    auto cellText = [&](const QJsonValue& cv) {
        return cv.isObject() ? cv.toObject().value(QStringLiteral("t")).toString() : cv.toString();
    };
    auto cellMedia = [&](const QJsonValue& cv) {
        return cv.isObject() ? cv.toObject().value(QStringLiteral("m")).toString() : QString();
    };
    while (cols > 1) {
        const int c = cols - 1;
        if (types[size_t(c)].kind != 0) break;
        bool empty = true;
        for (int r = 0; r < rows.size() && empty; ++r) {
            const QJsonArray cells = rows.at(r).toArray();
            if (c < cells.size()
                && (!cellText(cells.at(c)).isEmpty() || !cellMedia(cells.at(c)).isEmpty()))
                empty = false;
        }
        if (!empty) break;
        --cols;
    }

    // Column geometry — the app's BlockTable.recomputeAutoW, as the
    // minNotes exporter ports it: authored px width, else the widest
    // content line at the body size (+ header sort-glyph slot, choice
    // options), clamped 48..360, 160 when nothing is measurable.
    static const QFontMetricsF fm = [] {
        QFont f(chipFontFamily());
        f.setPixelSize(14);
        return QFontMetricsF(f);
    }();
    std::vector<double> widths(static_cast<size_t>(cols), 0.0);
    for (int c = 0; c < cols; ++c) {
        const int manual = colW(c);
        if (manual > 0) { widths[size_t(c)] = manual; continue; }
        const int kind = types[size_t(c)].kind;
        const int textRows = kind == 0 ? int(rows.size()) : header;
        double mw = 0;
        for (int r = 0; r < textRows; ++r) {
            const QJsonArray cells = rows.at(r).toArray();
            if (c >= cells.size()) continue;
            const double pad = (r == 0 && header > 0) ? 18 : 0;
            const QString t = cellText(cells.at(c)).left(2000);
            for (const QString& ln : t.split(QLatin1Char('\n')))
                mw = std::max(mw, fm.horizontalAdvance(ln) + pad);
        }
        if (kind == 1)
            for (const QJsonValue& ov : types[size_t(c)].options)
                mw = std::max(mw, fm.horizontalAdvance(
                    ov.toObject().value(QStringLiteral("l")).toString().left(200)) + 10);
        widths[size_t(c)] = mw <= 0 ? 160.0 : std::clamp(std::round(mw + 2 * 8 + 6), 48.0, 360.0);
    }
    double total = 0;
    int authored = 0;
    for (int c = 0; c < cols; ++c) {
        total += widths[size_t(c)];
        if (colW(c) > 0) ++authored;
    }
    QString colTags;
    QString tableOpen;
    QString wrapStyle;
    if (total > pageWidth) {
        // Wider than the measure: percentage columns + fixed layout, the
        // table keeps its full width capped by the viewport and escapes
        // the page column symmetrically (the app extends the sheet).
        for (int c = 0; c < cols; ++c)
            colTags += QStringLiteral("<col style=\"width:%1%\">")
                           .arg(widths[size_t(c)] / total * 100.0, 0, 'f', 2);
        tableOpen = QStringLiteral("<table style=\"table-layout:fixed;width:100%\">");
        const QString w = QStringLiteral("min(%1px,calc(100vw - 48px))").arg(int(total));
        wrapStyle = QStringLiteral(" style=\"width:%1;margin-left:calc((100% - %1)/2)\"").arg(w);
    } else {
        for (int c = 0; c < cols; ++c) {
            const int w = colW(c);
            colTags += w > 0 ? QStringLiteral("<col style=\"width:%1px\">").arg(w)
                             : QStringLiteral("<col>");
        }
        tableOpen = (authored == cols)
            ? QStringLiteral("<table style=\"table-layout:fixed\">")
            : QStringLiteral("<table>");
        if (authored == 0) colTags.clear();
    }
    QString out = QStringLiteral("<div class=\"tablewrap\"%1>").arg(wrapStyle) + tableOpen;
    if (!colTags.isEmpty())
        out += QStringLiteral("<colgroup>%1</colgroup>").arg(colTags);

    auto colorAt = [](const QJsonArray& arr, int i) {
        return i < arr.size() ? arr.at(i).toString() : QString();
    };

    for (int r = 0; r < rows.size(); ++r) {
        const QJsonArray cells = rows.at(r).toArray();
        const bool isHeader = r < header;
        out += QStringLiteral("<tr>");
        for (int c = 0; c < cols; ++c) {
            const QJsonValue cv = c < cells.size() ? cells.at(c) : QJsonValue(QString());
            const QJsonObject co = cv.isObject() ? cv.toObject() : QJsonObject();
            const QString text = cellText(cv);
            // Header cells stay TEXT in every column kind (the app's rule).
            const int kind = isHeader ? 0 : types[size_t(c)].kind;

            // Effective colors: cell → row → column, first non-empty wins.
            QString bg = co.value(QStringLiteral("bg")).toString();
            if (bg.isEmpty()) bg = colorAt(rbg, r);
            if (bg.isEmpty()) bg = colorAt(cbg, c);
            QString fg = co.value(QStringLiteral("fg")).toString();
            if (fg.isEmpty()) fg = colorAt(rfg, r);
            if (fg.isEmpty()) fg = colorAt(cfg, c);
            QString st;
            if (const QColor bc(bg); bc.isValid()) st += QStringLiteral("background:%1;").arg(bc.name());
            if (const QColor fc(fg); fc.isValid()) st += QStringLiteral("color:%1;").arg(fc.name());
            const int align = c < aArr.size() ? aArr.at(c).toInt(0) : 0;
            if (align == 1 || kind == 2) st += QStringLiteral("text-align:center;");
            else if (align == 2)         st += QStringLiteral("text-align:right;");

            QString inner;
            if (kind == 2) {
                inner = taskGlyph(std::clamp(co.value(QStringLiteral("v")).toString().toInt(), 0, 2));
            } else if (kind == 1) {
                const ChoicePick pick = pickChoice(types[size_t(c)].options,
                                                   co.value(QStringLiteral("v")).toString());
                inner = pick.found && !pick.label.isEmpty()
                    ? chipOpen(pick.color) + escapeHtml(pick.label) + QStringLiteral("</span>")
                    : QStringLiteral("<span class=\"chip unset\"></span>");
            } else {
                // Cell image rides above the text; sized to the column
                // (authored width − padding) or a 220px cap so an auto
                // column can't balloon to the image's intrinsic width.
                const QString m = co.value(QStringLiteral("m")).toString();
                if (!m.isEmpty()) {
                    const QJsonObject mo = QJsonDocument::fromJson(m.toUtf8()).object();
                    const QString abs = resolveMediaSrc(mo.value(QStringLiteral("src")), docDir);
                    if (!abs.isEmpty() && QFileInfo::exists(abs)) {
                        const int cw = colW(c);
                        const int cap = cw > 0 ? std::max(24, cw - 16) : 220;
                        int shown = mo.value(QStringLiteral("dw")).toInt(0);
                        if (shown <= 0) shown = mo.value(QStringLiteral("w")).toInt(0);
                        if (shown <= 0) shown = cap;
                        shown = std::min(shown, cap);
                        inner += QStringLiteral("<img class=\"cellimg\" src=\"%1\" alt=\"\" "
                                                "style=\"width:%2px\">")
                            .arg(QUrl::fromLocalFile(abs).toString(QUrl::FullyEncoded))
                            .arg(shown);
                    } else {
                        inner += QStringLiteral("<span class=\"mchip\">&#128444; %1</span>")
                            .arg(escapeHtml(QFileInfo(describeSrc(mo.value(QStringLiteral("src")))).fileName()));
                    }
                }
                // Cell spans (formatting + cell chips) ride the same walker;
                // cells can't carry comments, so cc never numbers here in
                // practice (an unknown thread id just falls through).
                inner += richText(text, co.value(QStringLiteral("s")).toArray(), cc);
            }
            const QString tag = isHeader ? QStringLiteral("th") : QStringLiteral("td");
            out += QStringLiteral("<%1%2>%3</%1>")
                       .arg(tag,
                            st.isEmpty() ? QString() : QStringLiteral(" style=\"%1\"").arg(st),
                            inner);
        }
        out += QStringLiteral("</tr>");
    }
    return out + QStringLiteral("</table></div>");
}

// ── .mnpkg packages ────────────────────────────────────────────────────
// minNotes' interchange package = a zip (vendored miniz reader) carrying
// `document.mndb` (DEFLATE) + a `media/` tree (STORE, the doc's
// `.minnotes/` assets under a dot-free name) + manifest.json. The preview
// stages it to a per-package temp dir laid out like an on-disk document
// (document.mndb + .minnotes/<asset>) so the renderer is untouched:
// every ".minnotes/…" src resolves against the stage dir. Only media
// the document actually references AND the renderer can show is
// extracted (the db comes out first and is scanned for ".minnotes/…"
// srcs; no video/audio/pdf/sidecars, nothing over 64 MB) — a 5 GB
// hand-off package with 300 assets stages in the time it takes to copy
// its handful of images; the rest still render as reference figures off
// the descriptor. The stage is keyed by package path and stamped with
// size+mtime so a re-saved package re-extracts.

bool packageEntryEscapes(const QString& name) {
    if (name.isEmpty()) return true;
    if (name.contains(QLatin1Char('\\'))) return true;
    if (name.startsWith(QLatin1Char('/'))) return true;
    if (name.size() >= 2 && name.at(1) == QLatin1Char(':')) return true;
    const QString clean = QDir::cleanPath(name);
    return clean == QLatin1String("..") || clean.startsWith(QLatin1String("../"))
        || clean.contains(QLatin1String("/../"));
}

bool packageEntryWanted(const QString& rel, qint64 size) {
    if (size < 0 || size > 64LL * 1024 * 1024) return false;
    if (rel.contains(QLatin1String("/.qcview/")) || rel.startsWith(QLatin1String(".qcview/")))
        return false;                                 // video-note sidecars
    static const QSet<QString> skip = {
        QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("m4v"),
        QStringLiteral("mkv"), QStringLiteral("avi"), QStringLiteral("webm"),
        QStringLiteral("mxf"), QStringLiteral("mts"), QStringLiteral("mp3"),
        QStringLiteral("wav"), QStringLiteral("aif"), QStringLiteral("aiff"),
        QStringLiteral("m4a"), QStringLiteral("flac"), QStringLiteral("pdf"),
        QStringLiteral("zip"), QStringLiteral("mnpkg"), QStringLiteral("mndb")};
    return !skip.contains(QFileInfo(rel).suffix().toLower());
}

// Returns the stage dir ("" on failure). `pkgPath` must exist.
QString stagePackage(const QString& pkgPath) {
    const QFileInfo pf(pkgPath);
    const QString stamp = QStringLiteral("%1:%2")
        .arg(pf.size()).arg(pf.lastModified().toMSecsSinceEpoch());
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/ufb-mnpkg-%1")
              .arg(QString::fromLatin1(QCryptographicHash::hash(
                  pkgPath.toUtf8(), QCryptographicHash::Sha1).toHex().left(16)));
    const QString dbOut = dir + QStringLiteral("/document.mndb");
    {
        QFile st(dir + QStringLiteral("/.stamp"));
        if (st.open(QIODevice::ReadOnly)
            && QString::fromUtf8(st.readAll()) == stamp
            && QFileInfo::exists(dbOut))
            return dir;                               // fresh stage
    }
    QDir(dir).removeRecursively();
    if (!QDir().mkpath(dir + QStringLiteral("/.minnotes"))) return {};

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, pkgPath.toUtf8().constData(), 0)) return {};
    auto fail = [&] { mz_zip_reader_end(&zip); QDir(dir).removeRecursively(); return QString(); };

    // Pass 1: the document itself.
    const int dbIdx = mz_zip_reader_locate_file(&zip, "document.mndb", nullptr, 0);
    if (dbIdx < 0
        || !mz_zip_reader_extract_to_file(&zip, mz_uint(dbIdx), dbOut.toUtf8().constData(), 0))
        return fail();

    // Which ".minnotes/<rel>" assets does it reference? One regex over the
    // block contents covers media/sketch-image/table-cell descriptors
    // alike (cell media is JSON-in-string, so the src ends at `\"`).
    QSet<QString> wanted;
    {
        const QString conn = QStringLiteral("mndb-stage-%1")
            .arg(QString::fromLatin1(QCryptographicHash::hash(
                pkgPath.toUtf8(), QCryptographicHash::Sha1).toHex().left(12)));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(dbOut);
            db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
            if (db.open()) {
                static const QRegularExpression ref(QStringLiteral("\\.minnotes/([^\"\\\\]+)"));
                QSqlQuery q(db);
                if (q.exec(QStringLiteral("SELECT content FROM blocks WHERE type='media' OR type='table'")))
                    while (q.next()) {
                        auto it = ref.globalMatch(q.value(0).toString());
                        while (it.hasNext()) wanted.insert(it.next().captured(1));
                    }
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(conn);
    }

    // Pass 2: referenced, showable media only.
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n && !wanted.isEmpty(); ++i) {
        mz_zip_archive_file_stat fs;
        if (!mz_zip_reader_file_stat(&zip, i, &fs) || fs.m_is_directory) continue;
        const QString name = QString::fromUtf8(fs.m_filename);
        if (!name.startsWith(QLatin1String("media/")) || packageEntryEscapes(name)) continue;
        const QString rel = name.mid(6);
        if (!wanted.contains(rel) || !packageEntryWanted(rel, qint64(fs.m_uncomp_size))) continue;
        const QString out = dir + QStringLiteral("/.minnotes/") + rel;
        // Belt and braces: the resolved path must stay inside the stage.
        if (!QDir::cleanPath(out).startsWith(dir + QLatin1Char('/'))) continue;
        QDir().mkpath(QFileInfo(out).absolutePath());
        mz_zip_reader_extract_to_file(&zip, i, out.toUtf8().constData(), 0);
    }
    mz_zip_reader_end(&zip);
    QFile st(dir + QStringLiteral("/.stamp"));
    if (st.open(QIODevice::WriteOnly | QIODevice::Truncate)) st.write(stamp.toUtf8());
    return dir;
}

QString commentStamp(qint64 t) {
    if (t <= 0) return {};
    // Tolerate both s and ms epochs.
    const QDateTime dt = t > 100000000000LL
        ? QDateTime::fromMSecsSinceEpoch(t)
        : QDateTime::fromSecsSinceEpoch(t);
    return dt.toString(QStringLiteral("yyyy-MM-dd hh:mm"));
}

// The preview's whole look — the minNotes export theme (dark sheet, the
// document's page measure, blue accent, squared corners everywhere) with
// the app's in-editor recipes where the export is plainer (heading sizes,
// chip borders, done-task strike, code language chip, header-row fill).
// %1 = page width px.
const char* kCss =
    ":root{--bg:#181817;--text:#e4e3e2;--bright:#f0f0f0;--muted:#8a8a8a;--subtle:#5e5e5e;"
    "--border:#2a2a2a;--divider:#333333;--accent:#0189f1;--recess:#0e0e0e;"
    "--chipbg:#1d2733;--chiptext:#4aa8ff;--codetext:#d4d4e8;--quote:#3a5e86;--surface:#121211}"
    "body{background:var(--bg);color:var(--text);margin:0;"
    "font:15px/1.65 -apple-system,BlinkMacSystemFont,'Segoe UI',Inter,Roboto,sans-serif}"
    "main{max-width:%1px;margin:0 auto;padding:40px 24px 96px}"
    "header.doctitle{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;"
    "color:var(--muted);padding-bottom:10px;margin-bottom:24px;border-bottom:1px solid var(--divider)}"
    "h1,h2,h3,h4,h5,h6{color:var(--bright);line-height:1.25;margin:1.1em 0 .45em}"
    "h1{font-size:30px}h2{font-size:26px}h3{font-size:22px}h4{font-size:19px}h5{font-size:17px}h6{font-size:16px}"
    "p{margin:.55em 0}"
    "a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}"
    "blockquote{font-family:Lora,Georgia,'Times New Roman',serif;border-left:3px solid var(--quote);"
    "margin:.6em 0;padding:2px 16px;color:var(--muted)}"
    "hr{border:none;border-top:1px solid var(--divider);margin:24px 0}"
    // Code: recessed block + the app's language chip pinned top-right.
    ".blkw{position:relative;margin:12px 0}"
    "pre{background:var(--recess);border:1px solid var(--border);margin:0;"
    "padding:14px 14px 12px;overflow-x:auto;font:12.5px/1.5 ui-monospace,'JetBrains Mono',Menlo,Consolas,monospace;"
    "color:var(--codetext)}"
    ".lang{position:absolute;right:8px;top:-8px;z-index:2;height:18px;line-height:16px;padding:0 8px;"
    "font:11px/16px ui-monospace,'JetBrains Mono',Menlo,Consolas,monospace;color:var(--muted);"
    "background:var(--surface);border:1px solid var(--border)}"
    ".lang.plain{color:var(--subtle)}"
    "code{font-family:ui-monospace,'JetBrains Mono',Menlo,Consolas,monospace;font-size:.9em}"
    ":not(pre)>code{background:var(--chipbg);color:var(--chiptext);padding:1px 5px}"
    // Lists: real nesting, 24px per depth (the app's step); app bullets.
    "ul,ol{padding-left:24px;margin:.3em 0}li{margin:2px 0;position:relative}"
    "ul{list-style:none}ul>li::before{content:'\\2022';color:var(--muted);position:absolute;left:-16px}"
    "ul>li.task::before{content:none}"
    "ul ul>li::before{content:'\\25E6'}ul ul ul>li::before{content:'\\2022'}"
    "ol{list-style:decimal}ol>li::marker{color:var(--muted)}"
    // Tri-state checkboxes — the app's exact recipe (squared, accent = state).
    ".cb{display:inline-block;width:14px;height:14px;box-sizing:border-box;position:relative;"
    "border:1.5px solid var(--muted);vertical-align:-2px;margin-right:6px}"
    ".cb.doing{border-color:var(--accent)}"
    ".cb.doing::after{content:'';position:absolute;left:2px;top:4.5px;width:7px;height:2px;background:var(--accent)}"
    ".cb.done{background:var(--accent);border:0}"
    ".cb.done::after{content:'';position:absolute;left:4.5px;top:1.5px;width:3.5px;height:7.5px;"
    "border:solid var(--bright);border-width:0 2px 2px 0;transform:rotate(45deg)}"
    ".tdone{color:var(--muted);text-decoration:line-through}"
    // Choice chips (inline, in cells, in choice columns): option color at
    // .28 fill / .55 border, squared, a little air around the label.
    ".chip{display:inline-block;padding:1px 8px;margin:0 1px;font-size:13px;line-height:1.45;"
    "color:var(--bright);background:#333333;border:1px solid #444;white-space:nowrap;"
    "vertical-align:baseline;font-weight:400;font-style:normal;text-decoration:none}"
    ".chip.unset{width:22px;height:18px;padding:0;background:transparent;border:1px solid var(--border);"
    "vertical-align:middle;position:relative}"
    ".chip.unset::after{content:'\\2304';position:absolute;left:6px;top:-4px;font-size:12px;color:var(--subtle)}"
    // Tables: hairline grid, header-row fill, cell images, squared.
    ".tablewrap{margin:20px 0;overflow-x:auto}"
    "table{border-collapse:collapse;width:max-content;min-width:100%;max-width:100%;font-size:14px;background:var(--bg)}"
    "td,th{border:1px solid var(--border);padding:6px 10px;text-align:left;vertical-align:top;overflow-wrap:break-word}"
    "th{background:#252525;color:var(--bright);font-weight:500}"
    "td .chip{font-size:13px}"
    "img.cellimg{display:block;max-width:100%;margin-bottom:4px}"
    // Media + reference figures.
    "figure{margin:16px 0}img{max-width:100%;display:block}"
    "figure.ref{border:1px solid var(--border);padding:0}"
    "figure.ref figcaption{padding:10px 14px}"
    ".fname{color:var(--bright)}"
    ".fpath{font-family:ui-monospace,Menlo,monospace;font-size:12px;color:var(--muted);word-break:break-all}"
    ".fmeta{font-family:ui-monospace,Menlo,monospace;font-size:12px;color:var(--subtle);margin-top:2px}"
    ".mchip{display:inline-block;background:var(--chipbg);color:var(--chiptext);"
    "padding:2px 8px;margin:4px 0;font-size:13px}"
    "img.sketch{background:transparent;max-width:100%}"
    // Ink stack (mirrors the minNotes export CSS): frame ink rides its
    // media; px ink is absolutely positioned inside its (relative) block.
    "main p,main h1,main h2,main h3,main h4,main h5,main h6,main blockquote,"
    "main li,main figure,main .tablewrap,main .blkw{position:relative}"
    "img.ink{pointer-events:none}"
    ".inkwrap{position:relative}"
    ".inkwrap img{display:block;max-width:100%}"
    ".inkwrap .ink{position:absolute;inset:0;width:100%;z-index:1;background:transparent}"
    // Comment ranges: tinted anchor (dimmed when resolved) + hover card.
    ".cref a{font-size:.72em;color:var(--accent)}"
    ".cmt{background:rgba(1,137,241,.13);position:relative}"
    ".cmt.resolved{background:rgba(140,140,140,.12)}"
    ".cmt .cmtcard{display:none;position:absolute;left:0;top:1.6em;z-index:30;"
    "width:300px;background:#202020;border:1px solid var(--border);padding:10px 12px;"
    "font-size:13px;font-style:normal;font-weight:400;line-height:1.5;color:var(--text);"
    "text-decoration:none}"
    ".cmt:hover .cmtcard{display:block}"
    ".cmtcard .cmsg{display:block;margin-bottom:6px}"
    ".comments{margin-top:48px;border-top:1px solid var(--divider);padding-top:12px}"
    ".comments h3{color:var(--bright);font-size:18px}"
    ".cthread{margin:10px 0;font-size:14px}"
    ".cthread.resolved{opacity:.55}"
    ".cmsg{margin:2px 0 2px 18px}"
    ".stamp{color:var(--subtle);font-size:11px;margin-left:8px}"
    ".resolved-tag{color:var(--muted);font-style:italic}";

}  // namespace

QString MndbDoc::htmlPreviewPath(const QString& mndbPath) const {
    if (mndbPath.isEmpty() || !QFileInfo::exists(mndbPath)) return {};
    // .mnpkg → stage to a temp document layout; everything below then
    // reads the staged document.mndb with the stage as its directory.
    const bool isPackage = mndbPath.endsWith(QLatin1String(".mnpkg"), Qt::CaseInsensitive);
    QString dbPath = mndbPath;
    QString docDir = QFileInfo(mndbPath).absolutePath();
    if (isPackage) {
        docDir = stagePackage(mndbPath);
        if (docDir.isEmpty()) return {};
        dbPath = docDir + QStringLiteral("/document.mndb");
    }

    // Unique connection per call; read-only so a doc open in minNotes
    // (or on SMB without shm backing) is never disturbed.
    const QString conn = QStringLiteral("mndb-preview-%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            mndbPath.toUtf8(), QCryptographicHash::Sha1).toHex().left(12)));
    QString body;
    QString title;
    QString commentsHtml;
    int pageWidth = 760;
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            // doc_meta.title is never written by minNotes (the file name is
            // the title); page_width is v3 (absent/0 → 760).
            QSqlQuery meta(db);
            if (meta.exec(QStringLiteral("SELECT title, page_width FROM doc_meta WHERE id=1"))
                && meta.next()) {
                title = meta.value(0).toString();
                const int pw = meta.value(1).toInt();
                if (pw > 0) pageWidth = std::clamp(pw, 400, 4000);
            } else {
                QSqlQuery meta2(db);   // pre-v3 doc without the column
                if (meta2.exec(QStringLiteral("SELECT title FROM doc_meta WHERE id=1"))
                    && meta2.next())
                    title = meta2.value(0).toString();
            }
            if (title.isEmpty()) title = QFileInfo(mndbPath).completeBaseName();

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
                // Real <ul>/<ol> nesting: one open list per depth level,
                // the minNotes export's listStack (bullet↔ordered at the
                // same depth closes and reopens).
                std::vector<QString> listStack;
                auto closeListsTo = [&](size_t n) {
                    while (listStack.size() > n) {
                        body += QStringLiteral("</%1>").arg(listStack.back());
                        listStack.pop_back();
                    }
                };
                while (q.next()) {
                    const QString blockId = q.value(0).toString();
                    const QString type = q.value(1).toString();
                    const QJsonObject attrs = QJsonDocument::fromJson(
                        q.value(2).toString().toUtf8()).object();
                    const QString content = q.value(3).toString();
                    const int depth = std::clamp(q.value(4).toInt(), 0, 8);
                    const bool isList = type == QLatin1String("list_item")
                        || type == QLatin1String("task_item")
                        || type == QLatin1String("ordered_item");
                    if (!isList) closeListsTo(0);

                    InkAnchor ink;
                    parseInk(inkByBlock.value(blockId), ink);

                    const QJsonArray spans =
                        attrs.value(QStringLiteral("spans")).toArray();
                    const auto inl = [&](const QString& t) { return richText(t, spans, cc); };

                    double indent = 0.0;    // block's own left offset in page px
                    QString blk;
                    bool needsWrap = false; // pre/hr can't host ink children
                    if (type == QLatin1String("heading")) {
                        const int lv = qBound(1, attrs.value(QStringLiteral("level")).toInt(1), 6);
                        blk = QStringLiteral("<h%1>%2</h%1>").arg(lv).arg(inl(content));
                    } else if (type == QLatin1String("quote")) {
                        blk = QStringLiteral("<blockquote>%1</blockquote>").arg(inl(content));
                    } else if (type == QLatin1String("code")) {
                        // The app's language chip (open string domain: KSyntax
                        // definition names and legacy fence tags alike).
                        const QString lang = attrs.value(QStringLiteral("lang")).toString().trimmed();
                        const QString chip = lang.isEmpty()
                            ? QStringLiteral("<span class=\"lang plain\">plain</span>")
                            : QStringLiteral("<span class=\"lang\">%1</span>").arg(escapeHtml(lang.left(40)));
                        blk = QStringLiteral("<div class=\"blkw\">%1<pre><code>%2</code></pre></div>")
                            .arg(chip, escapeHtml(content));
                    } else if (type == QLatin1String("divider")) {
                        blk = QStringLiteral("<hr>");
                        needsWrap = true;
                    } else if (type == QLatin1String("table")) {
                        blk = tableHtml(content, docDir, cc, pageWidth);
                    } else if (type == QLatin1String("media")) {
                        blk = mediaHtml(content, docDir, ink, pageWidth);
                    } else if (isList) {
                        const bool ordered = type == QLatin1String("ordered_item");
                        const QString tag = ordered ? QStringLiteral("ol") : QStringLiteral("ul");
                        const size_t want = size_t(depth) + 1;
                        closeListsTo(want);
                        if (listStack.size() == want && listStack.back() != tag)
                            closeListsTo(want - 1);
                        while (listStack.size() < want) {
                            body += QStringLiteral("<%1>").arg(tag);
                            listStack.push_back(tag);
                        }
                        QString li = inl(content);
                        if (type == QLatin1String("task_item")) {
                            const int st = std::clamp(attrs.value(QStringLiteral("state")).toInt(0), 0, 2);
                            if (st == 2)
                                li = QStringLiteral("<span class=\"tdone\">%1</span>").arg(li);
                            li = taskGlyph(st) + li;
                        }
                        indent = 24.0 * double(depth + 1);
                        blk = (type == QLatin1String("task_item")
                                   ? QStringLiteral("<li class=\"task\">%1</li>")
                                   : QStringLiteral("<li>%1</li>")).arg(li);
                    } else {  // paragraph + unknown future types degrade to text
                        blk = content.isEmpty()
                            ? QStringLiteral("<p>&nbsp;</p>")
                            : QStringLiteral("<p>%1</p>").arg(inl(content));
                    }
                    // (Comment anchors are emitted by the span walk itself:
                    // tinted range + hover card + superscript link.)

                    // Text-anchored margin ink: absolutely positioned inside
                    // the (position:relative) block, X from the page center
                    // minus the block's own indent.
                    const TextInk ti = renderTextInk(ink);
                    if (!ti.img.isNull()) {
                        const QString tag = QStringLiteral(
                            "<img class=\"ink\" style=\"position:absolute;left:%1px;"
                            "top:%2px;width:%3px;height:%4px;max-width:none;z-index:2\" src=\"%5\" alt=\"\">")
                            .arg(pageWidth / 2.0 + ti.box.left() - indent)
                            .arg(ti.box.top())
                            .arg(ti.box.width())
                            .arg(ti.box.height())
                            .arg(dataUri(ti.img));
                        if (needsWrap)
                            blk = QStringLiteral("<div class=\"blkw\">%1%2</div>").arg(blk, tag);
                        else
                            blk = insertBeforeClose(blk, tag);
                    }

                    body += blk;
                }
                closeListsTo(0);
            }

            if (!cc.order.isEmpty()) {
                commentsHtml = QStringLiteral(
                    "<section class=\"comments\"><h3>Comments</h3>");
                for (const QString& tid : cc.order) {
                    const bool res = cc.resolved.value(tid);
                    commentsHtml += QStringLiteral(
                        "<div class=\"cthread%4\" id=\"c%1\"><b>%1.</b>%2%3</div>")
                        .arg(cc.num.value(tid))
                        .arg(res ? QStringLiteral(" <span class=\"resolved-tag\">(resolved)</span>")
                                 : QString(),
                             cc.sectionMsgs.value(tid,
                                 QStringLiteral("<div class=\"cmsg\">(no messages)</div>")),
                             res ? QStringLiteral(" resolved") : QString());
                }
                commentsHtml += QStringLiteral("</section>");
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    if (!ok) return {};

    QString html = QStringLiteral("<!doctype html><meta charset=\"utf-8\"><style>%1</style><main>")
        .arg(QString::fromLatin1(kCss).arg(pageWidth));
    html += QStringLiteral("<header class=\"doctitle\">%1</header>").arg(escapeHtml(title));
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

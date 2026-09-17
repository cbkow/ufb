#include "DocxDoc.h"

#include "miniz.h"

#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QXmlStreamReader>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

namespace {

// Bump when the renderer's output changes — part of the stage stamp, so
// an app update re-renders instead of serving the old build's HTML.
constexpr const char* kRendererRev = "r2";

using X = QXmlStreamReader;

QString escapeHtml(QString s) {
    s.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    s.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    s.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    s.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    s.replace(QLatin1Char('\''), QStringLiteral("&#39;"));
    return s;
}

// Link targets are doc data and the page runs with JS on in a file://
// origin — and a rejected navigation is handed to the OS, so a link that
// resolves to a local path LAUNCHES that file. Web + mail only, decided
// on the exact string that is emitted: it must literally begin with the
// scheme. (Sniffing a whitespace-stripped copy is wrong — "ht tp:/../x"
// reads as http to the sniffer but Chromium keeps the space, sees no
// valid scheme and resolves it as a path relative to the file:// page.)
// Control characters anywhere disqualify; length-capped (the href repeats
// once per reference, so an unbounded one amplifies the output).
QString safeWebHref(const QString& target) {
    const QString t = target.trimmed();
    if (t.isEmpty() || t.size() > 2048) return {};
    for (const QChar c : t)
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) return {};
    static const QRegularExpression ok(QStringLiteral("^(https?://|mailto:)"),
                                       QRegularExpression::CaseInsensitiveOption);
    return ok.match(t).hasMatch() ? t : QString();
}

// Attribute by LOCAL name — tolerant of the transitional vs strict OOXML
// namespace URIs (and of writers that pick odd prefixes).
QString attr(const X& x, QLatin1String local) {
    for (const QXmlStreamAttribute& a : x.attributes())
        if (a.name() == local) return a.value().toString();
    return {};
}

// Same, restricted to the relationships namespace (r:id, r:embed) so a
// w:id on the same element can't be mistaken for it.
QString relAttr(const X& x, QLatin1String local) {
    for (const QXmlStreamAttribute& a : x.attributes())
        if (a.name() == local && a.namespaceUri().contains(QLatin1String("relationships")))
            return a.value().toString();
    return {};
}

// OOXML on/off: a bare <w:b/> is true; val 0/false/off (and "none" for
// underline) is false.
bool onOff(const X& x) {
    const QString v = attr(x, QLatin1String("val"));
    return !(v == QLatin1String("0") || v == QLatin1String("false")
             || v == QLatin1String("off") || v == QLatin1String("none"));
}

QString hexColor(const QString& v) {
    if (v.isEmpty() || v == QLatin1String("auto")) return {};
    const QColor c(QLatin1Char('#') + v);
    return c.isValid() ? c.name() : QString();
}

QString highlightColor(const QString& name) {
    static const QHash<QString, QString> map = {
        {QStringLiteral("yellow"), QStringLiteral("#ffff00")},
        {QStringLiteral("green"), QStringLiteral("#00ff00")},
        {QStringLiteral("cyan"), QStringLiteral("#00ffff")},
        {QStringLiteral("magenta"), QStringLiteral("#ff00ff")},
        {QStringLiteral("blue"), QStringLiteral("#0000ff")},
        {QStringLiteral("red"), QStringLiteral("#ff0000")},
        {QStringLiteral("darkBlue"), QStringLiteral("#000080")},
        {QStringLiteral("darkCyan"), QStringLiteral("#008080")},
        {QStringLiteral("darkGreen"), QStringLiteral("#008000")},
        {QStringLiteral("darkMagenta"), QStringLiteral("#800080")},
        {QStringLiteral("darkRed"), QStringLiteral("#800000")},
        {QStringLiteral("darkYellow"), QStringLiteral("#808000")},
        {QStringLiteral("darkGray"), QStringLiteral("#808080")},
        {QStringLiteral("lightGray"), QStringLiteral("#c0c0c0")},
        {QStringLiteral("black"), QStringLiteral("#000000")},
        {QStringLiteral("white"), QStringLiteral("#ffffff")}};
    return map.value(name);
}

// Font names land inside a quoted CSS font-family — allowlist the
// characters rather than escape (no quote, semicolon or backslash can
// survive), and cap the length.
QString safeFont(const QString& name) {
    QString out;
    for (const QChar c : name.left(64))
        if (c.isLetterOrNumber() || c == QLatin1Char(' ') || c == QLatin1Char('-')
            || c == QLatin1Char('_') || c == QLatin1Char('.'))
            out += c;
    return out.trimmed();
}

QString fontFamilyCss(const QString& font) {
    const QString f = font.toLower();
    const char* generic = "sans-serif";
    if (f.contains(QLatin1String("courier")) || f.contains(QLatin1String("consolas"))
        || f.contains(QLatin1String("mono")) || f.contains(QLatin1String("menlo")))
        generic = "monospace";
    else if (f.contains(QLatin1String("times")) || f.contains(QLatin1String("georgia"))
             || f.contains(QLatin1String("garamond")) || f.contains(QLatin1String("cambria"))
             || f.contains(QLatin1String("book")) || f.contains(QLatin1String("serif"))
             || f.contains(QLatin1String("palatino")) || f.contains(QLatin1String("minion")))
        generic = "serif";
    return QStringLiteral("font-family:'%1',%2").arg(font, QLatin1String(generic));
}

QString num(double v) { return QString::number(v, 'f', 2); }

// ── Properties ─────────────────────────────────────────────────────────

struct RunProps {
    std::optional<bool> b, i, u, strike, caps, smallCaps, vanish;
    QString color, highlight, font, styleId;
    double sizePt = 0;    // 0 = unset
    int vert = -1;        // -1 unset, 0 baseline, 1 superscript, 2 subscript

    void mergeFrom(const RunProps& o) {   // `o` wins where set
        if (o.b) b = o.b;
        if (o.i) i = o.i;
        if (o.u) u = o.u;
        if (o.strike) strike = o.strike;
        if (o.caps) caps = o.caps;
        if (o.smallCaps) smallCaps = o.smallCaps;
        if (o.vanish) vanish = o.vanish;
        if (!o.color.isEmpty()) color = o.color;
        if (!o.highlight.isEmpty()) highlight = o.highlight;
        if (!o.font.isEmpty()) font = o.font;
        if (o.sizePt > 0) sizePt = o.sizePt;
        if (o.vert >= 0) vert = o.vert;
    }

    QString css() const {
        QStringList d;
        if (b) d << (*b ? QStringLiteral("font-weight:bold") : QStringLiteral("font-weight:normal"));
        if (i) d << (*i ? QStringLiteral("font-style:italic") : QStringLiteral("font-style:normal"));
        if (u || strike) {
            QStringList lines;
            if (u && *u) lines << QStringLiteral("underline");
            if (strike && *strike) lines << QStringLiteral("line-through");
            d << QStringLiteral("text-decoration:%1")
                     .arg(lines.isEmpty() ? QStringLiteral("none") : lines.join(QLatin1Char(' ')));
        }
        if (!color.isEmpty()) d << QStringLiteral("color:%1").arg(color);
        if (!highlight.isEmpty()) d << QStringLiteral("background:%1").arg(highlight);
        if (!font.isEmpty()) d << fontFamilyCss(font);
        if (sizePt > 0 && vert <= 0) d << QStringLiteral("font-size:%1pt").arg(num(sizePt));
        if (vert == 1) d << QStringLiteral("vertical-align:super;font-size:65%");
        if (vert == 2) d << QStringLiteral("vertical-align:sub;font-size:65%");
        if (caps && *caps) d << QStringLiteral("text-transform:uppercase");
        if (smallCaps && *smallCaps) d << QStringLiteral("font-variant:small-caps");
        return d.join(QLatin1Char(';'));
    }
};

struct ParaProps {
    QString styleId, jc, shade, lineRule;
    int numId = -1, ilvl = -1, outline = -1;   // -1 = unset
    std::optional<double> indLeft, indRight, indFirst;   // twips; indFirst < 0 = hanging
    std::optional<double> before, after, line;           // twips (line: 240ths when auto)
    std::optional<bool> pageBreakBefore;
    bool borderTop = false, borderBottom = false;

    void mergeFrom(const ParaProps& o) {
        if (!o.jc.isEmpty()) jc = o.jc;
        if (!o.shade.isEmpty()) shade = o.shade;
        if (o.numId >= 0) numId = o.numId;
        if (o.ilvl >= 0) ilvl = o.ilvl;
        if (o.outline >= 0) outline = o.outline;
        if (o.indLeft) indLeft = o.indLeft;
        if (o.indRight) indRight = o.indRight;
        if (o.indFirst) indFirst = o.indFirst;
        if (o.before) before = o.before;
        if (o.after) after = o.after;
        if (o.line) { line = o.line; lineRule = o.lineRule; }
        if (o.pageBreakBefore) pageBreakBefore = o.pageBreakBefore;
        if (o.borderTop) borderTop = true;
        if (o.borderBottom) borderBottom = true;
    }
};

struct Style {
    QString id, name, basedOn, type;
    ParaProps p;
    RunProps r;
    std::optional<bool> tblBorders;
    bool isDefault = false;
};

struct PageGeom { double w = 12240, left = 1440, right = 1440, top = 1440, bottom = 1440; };

bool borderVisible(const X& x) {
    const QString v = attr(x, QLatin1String("val"));
    return !v.isEmpty() && v != QLatin1String("none") && v != QLatin1String("nil");
}

// Each parse* below is entered ON the element's start tag and returns ON
// its end tag (the readNextStartElement loop runs the element dry).
// Callers must then `continue`, never skipCurrentElement — skipping from
// an end tag would swallow the rest of the PARENT.

void parseRPr(X& x, RunProps& rp, const QString& majorFont, const QString& minorFont) {
    while (x.readNextStartElement()) {
        const auto n = x.name();
        if (n == QLatin1String("b")) rp.b = onOff(x);
        else if (n == QLatin1String("i")) rp.i = onOff(x);
        else if (n == QLatin1String("u")) rp.u = onOff(x);
        else if (n == QLatin1String("strike") || n == QLatin1String("dstrike")) rp.strike = onOff(x);
        else if (n == QLatin1String("caps")) rp.caps = onOff(x);
        else if (n == QLatin1String("smallCaps")) rp.smallCaps = onOff(x);
        else if (n == QLatin1String("vanish")) rp.vanish = onOff(x);
        else if (n == QLatin1String("rStyle")) rp.styleId = attr(x, QLatin1String("val"));
        else if (n == QLatin1String("color")) rp.color = hexColor(attr(x, QLatin1String("val")));
        else if (n == QLatin1String("highlight")) rp.highlight = highlightColor(attr(x, QLatin1String("val")));
        else if (n == QLatin1String("shd")) {
            const QString f = hexColor(attr(x, QLatin1String("fill")));
            if (!f.isEmpty() && rp.highlight.isEmpty()) rp.highlight = f;
        } else if (n == QLatin1String("sz")) {
            const double half = attr(x, QLatin1String("val")).toDouble();
            if (half >= 2 && half <= 800) rp.sizePt = half / 2.0;
        } else if (n == QLatin1String("vertAlign")) {
            const QString v = attr(x, QLatin1String("val"));
            rp.vert = v == QLatin1String("superscript") ? 1 : v == QLatin1String("subscript") ? 2 : 0;
        } else if (n == QLatin1String("rFonts")) {
            QString f = safeFont(attr(x, QLatin1String("ascii")));
            if (f.isEmpty()) {
                const QString theme = attr(x, QLatin1String("asciiTheme"));
                if (theme.startsWith(QLatin1String("major"))) f = majorFont;
                else if (theme.startsWith(QLatin1String("minor"))) f = minorFont;
            }
            if (!f.isEmpty()) rp.font = f;
        }
        x.skipCurrentElement();
    }
}

void parseSect(X& x, PageGeom& page) {
    while (x.readNextStartElement()) {
        const auto n = x.name();
        if (n == QLatin1String("pgSz")) {
            const double w = attr(x, QLatin1String("w")).toDouble();
            if (w > 0) page.w = w;
        } else if (n == QLatin1String("pgMar")) {
            auto take = [&](QLatin1String a, double& out) {
                bool ok = false;
                const double v = attr(x, a).toDouble(&ok);
                if (ok && v >= 0) out = v;
            };
            take(QLatin1String("left"), page.left);
            take(QLatin1String("right"), page.right);
            take(QLatin1String("top"), page.top);
            take(QLatin1String("bottom"), page.bottom);
        }
        x.skipCurrentElement();
    }
}

void parsePPr(X& x, ParaProps& pp) {
    auto twips = [&](QLatin1String a) -> std::optional<double> {
        const QString v = attr(x, a);
        if (v.isEmpty()) return std::nullopt;
        bool ok = false;
        const double d = v.toDouble(&ok);
        if (!ok || !std::isfinite(d)) return std::nullopt;
        return std::clamp(d, -31680.0, 31680.0);
    };
    while (x.readNextStartElement()) {
        const auto n = x.name();
        if (n == QLatin1String("pStyle")) pp.styleId = attr(x, QLatin1String("val"));
        else if (n == QLatin1String("jc")) pp.jc = attr(x, QLatin1String("val"));
        else if (n == QLatin1String("outlineLvl")) pp.outline = attr(x, QLatin1String("val")).toInt();
        else if (n == QLatin1String("pageBreakBefore")) pp.pageBreakBefore = onOff(x);
        else if (n == QLatin1String("shd")) pp.shade = hexColor(attr(x, QLatin1String("fill")));
        else if (n == QLatin1String("ind")) {
            if (auto v = twips(QLatin1String("left"))) pp.indLeft = v;
            else if (auto s = twips(QLatin1String("start"))) pp.indLeft = s;
            if (auto v = twips(QLatin1String("right"))) pp.indRight = v;
            else if (auto e = twips(QLatin1String("end"))) pp.indRight = e;
            if (auto h = twips(QLatin1String("hanging"))) pp.indFirst = -*h;
            else if (auto f = twips(QLatin1String("firstLine"))) pp.indFirst = f;
        } else if (n == QLatin1String("spacing")) {
            if (auto v = twips(QLatin1String("before"))) pp.before = v;
            if (auto v = twips(QLatin1String("after"))) pp.after = v;
            if (auto v = twips(QLatin1String("line"))) {
                pp.line = v;
                pp.lineRule = attr(x, QLatin1String("lineRule"));
            }
        } else if (n == QLatin1String("numPr")) {
            while (x.readNextStartElement()) {
                if (x.name() == QLatin1String("ilvl"))
                    pp.ilvl = std::clamp(attr(x, QLatin1String("val")).toInt(), 0, 8);
                else if (x.name() == QLatin1String("numId"))
                    pp.numId = std::max(0, attr(x, QLatin1String("val")).toInt());
                x.skipCurrentElement();
            }
            continue;
        } else if (n == QLatin1String("pBdr")) {
            while (x.readNextStartElement()) {
                if (x.name() == QLatin1String("top") && borderVisible(x)) pp.borderTop = true;
                else if (x.name() == QLatin1String("bottom") && borderVisible(x)) pp.borderBottom = true;
                x.skipCurrentElement();
            }
            continue;
        }
        // (A sectPr here closes a mid-document section — skipped like the
        // rest: the page we frame is the body's final sectPr.)
        x.skipCurrentElement();
    }
}

// ── Package parts ──────────────────────────────────────────────────────

struct Rel { QString target, type; bool external = false; };

struct Level {
    int start = 1;
    QString fmt = QStringLiteral("decimal"), text, pStyle;
    std::optional<double> indLeft, indFirst;
};
struct AbstractNum { std::array<Level, 9> lvl; };
struct NumInst { int abstractId = -1; QHash<int, int> startOverride; };

struct Package {
    mz_zip_archive zip;
    bool open = false;
    QString baseDir;                       // dir of the main part ("word")
    QHash<QString, Rel> rels;              // main part's relationships
    QHash<QString, Style> styles;
    QString defaultParaStyle;
    ParaProps defaultP;
    RunProps defaultR;
    QString majorFont, minorFont;
    QHash<int, AbstractNum> abstractNums;
    QHash<int, NumInst> nums;

    ~Package() { if (open) mz_zip_reader_end(&zip); }

    QByteArray read(const QString& entry, quint64 cap) {
        const int idx = mz_zip_reader_locate_file(&zip, entry.toUtf8().constData(), nullptr, 0);
        mz_zip_archive_file_stat st;
        if (idx < 0 || !mz_zip_reader_file_stat(&zip, mz_uint(idx), &st)
            || st.m_is_directory || st.m_uncomp_size > cap)
            return {};
        QByteArray buf(qsizetype(st.m_uncomp_size), Qt::Uninitialized);
        if (!buf.isEmpty()
            && !mz_zip_reader_extract_to_mem(&zip, mz_uint(idx), buf.data(), size_t(buf.size()), 0))
            return {};
        return buf;
    }
};

// A relationship target → a package entry name, or "" when it would
// leave the package. Absolute ("/word/media/x.png") and relative
// ("media/x.png", "../media/x.png") forms both occur in the wild.
QString resolvePart(const QString& baseDir, const QString& target) {
    if (target.isEmpty() || target.contains(QLatin1Char('\\')) || target.contains(QLatin1Char(':')))
        return {};
    const QString joined = target.startsWith(QLatin1Char('/'))
        ? target.mid(1)
        : (baseDir.isEmpty() ? target : baseDir + QLatin1Char('/') + target);
    const QString clean = QDir::cleanPath(joined);
    if (clean.isEmpty() || clean.startsWith(QLatin1String("..")) || clean.startsWith(QLatin1Char('/')))
        return {};
    return clean;
}

QHash<QString, Rel> parseRels(const QByteArray& xml) {
    QHash<QString, Rel> out;
    X x(xml);
    while (!x.atEnd()) {
        if (x.readNext() != X::StartElement || x.name() != QLatin1String("Relationship")) continue;
        Rel r;
        r.target = attr(x, QLatin1String("Target"));
        r.type = attr(x, QLatin1String("Type"));
        r.external = attr(x, QLatin1String("TargetMode")).compare(
                         QLatin1String("External"), Qt::CaseInsensitive) == 0;
        const QString id = attr(x, QLatin1String("Id"));
        if (!id.isEmpty() && out.size() < 20000) out.insert(id, r);
    }
    return out;
}

QString relTargetByType(const QHash<QString, Rel>& rels, QLatin1String suffix) {
    for (const Rel& r : rels)
        if (!r.external && r.type.endsWith(suffix)) return r.target;
    return {};
}

void parseTheme(Package& pkg, const QByteArray& xml) {
    X x(xml);
    int which = 0;   // 1 major, 2 minor
    while (!x.atEnd()) {
        const auto t = x.readNext();
        if (t == X::StartElement) {
            if (x.name() == QLatin1String("majorFont")) which = 1;
            else if (x.name() == QLatin1String("minorFont")) which = 2;
            else if (x.name() == QLatin1String("latin") && which) {
                const QString f = safeFont(attr(x, QLatin1String("typeface")));
                if (which == 1 && pkg.majorFont.isEmpty()) pkg.majorFont = f;
                if (which == 2 && pkg.minorFont.isEmpty()) pkg.minorFont = f;
            }
        } else if (t == X::EndElement
                   && (x.name() == QLatin1String("majorFont") || x.name() == QLatin1String("minorFont"))) {
            which = 0;
        }
    }
}

void parseStyles(Package& pkg, const QByteArray& xml) {
    X x(xml);
    if (!x.readNextStartElement()) return;   // <w:styles>
    while (x.readNextStartElement()) {
        const auto n = x.name();
        if (n == QLatin1String("docDefaults")) {
            while (x.readNextStartElement()) {
                const bool isR = x.name() == QLatin1String("rPrDefault");
                const bool isP = x.name() == QLatin1String("pPrDefault");
                if (!isR && !isP) { x.skipCurrentElement(); continue; }
                while (x.readNextStartElement()) {
                    if (isR && x.name() == QLatin1String("rPr"))
                        parseRPr(x, pkg.defaultR, pkg.majorFont, pkg.minorFont);
                    else if (isP && x.name() == QLatin1String("pPr"))
                        parsePPr(x, pkg.defaultP);
                    else
                        x.skipCurrentElement();
                }
            }
            continue;
        }
        if (n != QLatin1String("style")) { x.skipCurrentElement(); continue; }
        Style st;
        st.id = attr(x, QLatin1String("styleId"));
        st.type = attr(x, QLatin1String("type"));
        const QString def = attr(x, QLatin1String("default"));
        st.isDefault = def == QLatin1String("1") || def == QLatin1String("true");
        while (x.readNextStartElement()) {
            const auto c = x.name();
            if (c == QLatin1String("name")) st.name = attr(x, QLatin1String("val"));
            else if (c == QLatin1String("basedOn")) st.basedOn = attr(x, QLatin1String("val"));
            else if (c == QLatin1String("pPr")) { parsePPr(x, st.p); continue; }
            else if (c == QLatin1String("rPr")) {
                parseRPr(x, st.r, pkg.majorFont, pkg.minorFont);
                continue;
            } else if (c == QLatin1String("tblPr")) {
                while (x.readNextStartElement()) {
                    if (x.name() != QLatin1String("tblBorders")) { x.skipCurrentElement(); continue; }
                    bool any = false;
                    while (x.readNextStartElement()) {
                        if (borderVisible(x)) any = true;
                        x.skipCurrentElement();
                    }
                    st.tblBorders = any;
                }
                continue;
            }
            x.skipCurrentElement();
        }
        if (st.id.isEmpty() || pkg.styles.size() >= 5000) continue;
        if (st.isDefault && st.type == QLatin1String("paragraph")) pkg.defaultParaStyle = st.id;
        pkg.styles.insert(st.id, st);
    }
}

void parseNumbering(Package& pkg, const QByteArray& xml) {
    X x(xml);
    if (!x.readNextStartElement()) return;   // <w:numbering>
    while (x.readNextStartElement()) {
        const auto n = x.name();
        if (n == QLatin1String("abstractNum")) {
            const int id = attr(x, QLatin1String("abstractNumId")).toInt();
            AbstractNum an;
            while (x.readNextStartElement()) {
                if (x.name() != QLatin1String("lvl")) { x.skipCurrentElement(); continue; }
                const int il = attr(x, QLatin1String("ilvl")).toInt();
                Level lv;
                while (x.readNextStartElement()) {
                    const auto c = x.name();
                    if (c == QLatin1String("start")) lv.start = std::clamp(attr(x, QLatin1String("val")).toInt(), 0, 1000000);
                    else if (c == QLatin1String("numFmt")) lv.fmt = attr(x, QLatin1String("val"));
                    else if (c == QLatin1String("lvlText")) lv.text = attr(x, QLatin1String("val")).left(64);
                    else if (c == QLatin1String("pStyle")) lv.pStyle = attr(x, QLatin1String("val"));
                    else if (c == QLatin1String("pPr")) {
                        ParaProps pp;
                        parsePPr(x, pp);
                        lv.indLeft = pp.indLeft;
                        lv.indFirst = pp.indFirst;
                        continue;
                    }
                    x.skipCurrentElement();
                }
                if (il >= 0 && il < 9) an.lvl[size_t(il)] = lv;
            }
            if (pkg.abstractNums.size() < 5000) pkg.abstractNums.insert(id, an);
            continue;
        }
        if (n == QLatin1String("num")) {
            const int id = attr(x, QLatin1String("numId")).toInt();
            NumInst ni;
            while (x.readNextStartElement()) {
                if (x.name() == QLatin1String("abstractNumId")) {
                    ni.abstractId = attr(x, QLatin1String("val")).toInt();
                } else if (x.name() == QLatin1String("lvlOverride")) {
                    const int il = attr(x, QLatin1String("ilvl")).toInt();
                    while (x.readNextStartElement()) {
                        if (x.name() == QLatin1String("startOverride"))
                            ni.startOverride.insert(il, std::clamp(attr(x, QLatin1String("val")).toInt(), 0, 1000000));
                        x.skipCurrentElement();
                    }
                    continue;
                }
                x.skipCurrentElement();
            }
            if (pkg.nums.size() < 20000) pkg.nums.insert(id, ni);
            continue;
        }
        x.skipCurrentElement();
    }
}

// ── List markers ───────────────────────────────────────────────────────

QString roman(int n) {
    if (n <= 0 || n >= 4000) return QString::number(n);
    static const int v[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char* s[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
    QString out;
    for (int k = 0; k < 13; ++k)
        while (n >= v[k]) { out += QLatin1String(s[k]); n -= v[k]; }
    return out;
}

QString letters(int n) {   // 1→a … 26→z, 27→aa (Word repeats the letter)
    if (n <= 0) return QString::number(n);
    const QChar c(u'a' + (n - 1) % 26);
    return QString(std::min((n - 1) / 26 + 1, 8), c);
}

QString formatCounter(int n, const QString& fmt) {
    if (fmt == QLatin1String("lowerLetter")) return letters(n);
    if (fmt == QLatin1String("upperLetter")) return letters(n).toUpper();
    if (fmt == QLatin1String("lowerRoman")) return roman(n).toLower();
    if (fmt == QLatin1String("upperRoman")) return roman(n);
    if (fmt == QLatin1String("decimalZero")) return QStringLiteral("%1").arg(n, 2, 10, QLatin1Char('0'));
    if (fmt == QLatin1String("none")) return {};
    return QString::number(n);
}

// Bullet glyphs are usually Symbol/Wingdings private-use code points that
// mean nothing in a web font — map the common ones, default the rest.
QString bulletGlyph(const QString& text, int ilvl) {
    if (text.isEmpty()) return QStringLiteral("•");
    const char16_t c = text.at(0).unicode();
    switch (c) {
    case 0xF0B7: case 0x00B7: return QStringLiteral("•");
    case 0xF0A7: return QStringLiteral("▪");
    case 0xF0D8: return QStringLiteral("➢");
    case 0xF0FC: return QStringLiteral("✓");
    case 0xF076: return QStringLiteral("❖");
    case u'o':   return QStringLiteral("◦");
    default: break;
    }
    if (c >= 0xE000 && c <= 0xF8FF)
        return ilvl % 3 == 1 ? QStringLiteral("◦")
             : ilvl % 3 == 2 ? QStringLiteral("▪") : QStringLiteral("•");
    return text.left(4);
}

// ── Renderer ───────────────────────────────────────────────────────────

struct Cell {
    QString html, fill, valign;
    int span = 1, vmerge = 0;   // vmerge: 0 none, 1 restart, 2 continue
    double width = 0;           // tcW in twips (0 = unset / not dxa)
    int gridCol = 0, rowspan = 1;
    bool merged = false;        // swallowed by a rowspan above
};

class Renderer {
public:
    Renderer(Package& pkg, const QString& stageDir) : pkg_(pkg), stage_(stageDir) {}

    PageGeom page;
    // truncated: a budget ran out — everything after is skipped, not
    // rendered. omitted: something too deep/wide was dropped in place.
    // Either way the page says so.
    bool truncated = false;
    bool omitted = false;

    // Entered on a container's start tag (body, tc, txbxContent); returns
    // on its end tag.
    QString blocks(X& x, Cell* cell = nullptr) {
        QString html;
        int depth = 0;   // transparent wrappers we descended into
        while (!x.atEnd()) {
            const auto t = x.readNext();
            if (t == X::StartElement) {
                const auto n = x.name();
                if ((n == QLatin1String("p") || n == QLatin1String("tbl")) && truncated)
                    x.skipCurrentElement();
                else if (n == QLatin1String("p")) html += paragraph(x);
                else if (n == QLatin1String("tbl")) html += table(x);
                else if (n == QLatin1String("tcPr") && cell) parseTcPr(x, *cell);
                else if (n == QLatin1String("sectPr")) parseSect(x, page);
                else if (n == QLatin1String("sdt") || n == QLatin1String("sdtContent")
                         || n == QLatin1String("customXml") || n == QLatin1String("smartTag")
                         || n == QLatin1String("ins") || n == QLatin1String("moveTo"))
                    ++depth;
                else
                    x.skipCurrentElement();
            } else if (t == X::EndElement) {
                if (depth == 0) break;
                --depth;
            }
        }
        return html;
    }

private:
    Package& pkg_;
    QString stage_;
    // Output budgets. The paragraph count alone bounds nothing: one
    // paragraph can hold millions of references that each expand to a
    // long tag (a 2 KB docx made 100 MB of HTML on the GUI thread).
    static constexpr qsizetype kInlineBudget = 4 * 1024 * 1024;    // chars per paragraph / run
    static constexpr qint64 kOutputBudget = 48LL * 1024 * 1024;    // chars per document
    qint64 emitted_ = 0;
    int paragraphs_ = 0;
    int tableDepth_ = 0;
    int boxDepth_ = 0;
    int images_ = 0;
    qint64 imageBudget_ = 256LL * 1024 * 1024;
    QHash<QString, QString> imageByEntry_;   // zip entry → staged relative src
    struct Counters { std::array<int, 9> n{}; std::array<bool, 9> used{}; };
    QHash<int, Counters> counters_;          // by abstractNumId
    QSet<int> seenNums_;

    // Style chain → merged props (docDefaults first). Depth-capped: a
    // basedOn cycle must not spin.
    void resolveStyle(const QString& id, ParaProps& p, RunProps& r) const {
        std::vector<const Style*> chain;
        QString cur = id;
        for (int guard = 0; guard < 16 && !cur.isEmpty(); ++guard) {
            const auto it = pkg_.styles.constFind(cur);
            if (it == pkg_.styles.constEnd()) break;
            chain.push_back(&*it);
            cur = it->basedOn;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            p.mergeFrom((*it)->p);
            r.mergeFrom((*it)->r);
        }
    }

    int headingLevel(const QString& styleId, const ParaProps& merged) const {
        QString cur = styleId;
        for (int guard = 0; guard < 16 && !cur.isEmpty(); ++guard) {
            const auto it = pkg_.styles.constFind(cur);
            if (it == pkg_.styles.constEnd()) break;
            static const QRegularExpression re(QStringLiteral("^heading\\s*([1-9])$"),
                                               QRegularExpression::CaseInsensitiveOption);
            const auto m = re.match(it->name);
            if (m.hasMatch()) return std::min(m.captured(1).toInt(), 6);
            if (it->name.compare(QLatin1String("Title"), Qt::CaseInsensitive) == 0) return 1;
            cur = it->basedOn;
        }
        if (merged.outline >= 0 && merged.outline <= 5) return merged.outline + 1;
        return 0;
    }

    // Advances the list counters and returns the marker text ("" = none).
    QString listMarker(int numId, int ilvl, const Level** levelOut) {
        const auto ni = pkg_.nums.constFind(numId);
        if (ni == pkg_.nums.constEnd()) return {};
        const auto an = pkg_.abstractNums.constFind(ni->abstractId);
        if (an == pkg_.abstractNums.constEnd()) return {};
        Counters& c = counters_[ni->abstractId];
        if (!seenNums_.contains(numId)) {
            seenNums_.insert(numId);
            // A restart override re-arms those levels for this instance.
            for (auto it = ni->startOverride.constBegin(); it != ni->startOverride.constEnd(); ++it)
                if (it.key() >= 0 && it.key() < 9) c.used[size_t(it.key())] = false;
        }
        auto startOf = [&](int l) {
            return ni->startOverride.value(l, an->lvl[size_t(l)].start);
        };
        const size_t L = size_t(ilvl);
        if (!c.used[L]) { c.n[L] = startOf(ilvl) - 1; c.used[L] = true; }
        ++c.n[L];
        for (size_t j = L + 1; j < 9; ++j) c.used[j] = false;

        const Level& lv = an->lvl[L];
        if (levelOut) *levelOut = &lv;
        if (lv.fmt == QLatin1String("bullet")) return bulletGlyph(lv.text, ilvl);
        QString out;
        for (int k = 0; k < lv.text.size(); ++k) {
            const QChar ch = lv.text.at(k);
            if (ch == QLatin1Char('%') && k + 1 < lv.text.size() && lv.text.at(k + 1).isDigit()) {
                const int ref = lv.text.at(k + 1).digitValue() - 1;
                if (ref >= 0 && ref < 9) {
                    const size_t R = size_t(ref);
                    const int v = c.used[R] ? c.n[R] : startOf(ref);
                    out += formatCounter(v, an->lvl[R].fmt);
                }
                ++k;
            } else {
                out += ch;
            }
        }
        return out;
    }

    QString paragraph(X& x) {
        ParaProps direct;
        QString inner, after;
        std::vector<QString> closers;
        while (!x.atEnd()) {
            const auto t = x.readNext();
            if (t == X::StartElement) {
                const auto n = x.name();
                if (inner.size() > kInlineBudget) { truncated = true; x.skipCurrentElement(); continue; }
                if (n == QLatin1String("pPr")) parsePPr(x, direct);
                else if (n == QLatin1String("r")) inner += run(x, after);
                else if (n == QLatin1String("hyperlink")) {
                    QString href;
                    const auto rel = pkg_.rels.constFind(relAttr(x, QLatin1String("id")));
                    if (rel != pkg_.rels.constEnd()) href = safeWebHref(rel->target);
                    if (href.isEmpty()) { inner += QStringLiteral("<span>"); closers.push_back(QStringLiteral("</span>")); }
                    else {
                        inner += QStringLiteral("<a href=\"%1\">").arg(escapeHtml(href));
                        closers.push_back(QStringLiteral("</a>"));
                    }
                } else if (n == QLatin1String("ins") || n == QLatin1String("moveTo")
                           || n == QLatin1String("smartTag") || n == QLatin1String("sdt")
                           || n == QLatin1String("sdtContent") || n == QLatin1String("fldSimple")
                           || n == QLatin1String("customXml") || n == QLatin1String("dir")
                           || n == QLatin1String("bdo")) {
                    closers.push_back(QString());
                } else {
                    x.skipCurrentElement();   // del, moveFrom, bookmarks, sdtPr, math, …
                }
            } else if (t == X::EndElement) {
                if (closers.empty()) break;
                inner += closers.back();
                closers.pop_back();
            }
        }
        if (++paragraphs_ > 60000) { truncated = true; return {}; }

        ParaProps pp = pkg_.defaultP;
        RunProps rp = pkg_.defaultR;
        const QString styleId = direct.styleId.isEmpty() ? pkg_.defaultParaStyle : direct.styleId;
        resolveStyle(styleId, pp, rp);
        const ParaProps styled = pp;
        pp.mergeFrom(direct);

        // Numbering: the level's indents sit between the style's and the
        // paragraph's own.
        QString marker;
        const Level* level = nullptr;
        if (pp.numId > 0) {
            int ilvl = pp.ilvl;
            if (ilvl < 0) {   // style-linked multilevel list: the level names the style
                ilvl = 0;
                const auto ni = pkg_.nums.constFind(pp.numId);
                if (ni != pkg_.nums.constEnd()) {
                    const auto an = pkg_.abstractNums.constFind(ni->abstractId);
                    if (an != pkg_.abstractNums.constEnd())
                        for (int l = 0; l < 9; ++l)
                            if (!styleId.isEmpty() && an->lvl[size_t(l)].pStyle == styleId) { ilvl = l; break; }
                }
            }
            marker = listMarker(pp.numId, ilvl, &level);
            if (level) {
                if (!direct.indLeft && level->indLeft) pp.indLeft = level->indLeft;
                if (!direct.indFirst && level->indFirst) pp.indFirst = level->indFirst;
                if (!pp.indLeft && !styled.indLeft) pp.indLeft = 720.0 * (ilvl + 1);
                if (!pp.indFirst) pp.indFirst = -360.0;
            }
        }

        QStringList css;
        const double left = pp.indLeft.value_or(0) / 15.0;
        const double first = pp.indFirst.value_or(0) / 15.0;
        if (left != 0) css << QStringLiteral("margin-left:%1px").arg(num(left));
        if (pp.indRight && *pp.indRight != 0)
            css << QStringLiteral("margin-right:%1px").arg(num(*pp.indRight / 15.0));
        if (first != 0) css << QStringLiteral("text-indent:%1px").arg(num(first));
        css << QStringLiteral("margin-top:%1pt").arg(num(std::clamp(pp.before.value_or(0) / 20.0, 0.0, 200.0)));
        css << QStringLiteral("margin-bottom:%1pt").arg(num(std::clamp(pp.after.value_or(0) / 20.0, 0.0, 200.0)));
        if (pp.line && *pp.line > 0) {
            if (pp.lineRule == QLatin1String("exact"))
                css << QStringLiteral("line-height:%1pt").arg(num(std::clamp(*pp.line / 20.0, 4.0, 400.0)));
            else if (pp.lineRule != QLatin1String("atLeast"))   // auto: 240 = single ≈ 1.2em
                css << QStringLiteral("line-height:%1").arg(num(std::clamp(*pp.line / 240.0 * 1.2, 0.8, 6.0)));
        }
        if (pp.jc == QLatin1String("center")) css << QStringLiteral("text-align:center");
        else if (pp.jc == QLatin1String("right") || pp.jc == QLatin1String("end")) css << QStringLiteral("text-align:right");
        else if (pp.jc == QLatin1String("both") || pp.jc == QLatin1String("distribute")) css << QStringLiteral("text-align:justify");
        if (!pp.shade.isEmpty()) css << QStringLiteral("background:%1").arg(pp.shade);
        if (pp.borderTop) css << QStringLiteral("border-top:1px solid #808080;padding-top:1pt");
        if (pp.borderBottom) css << QStringLiteral("border-bottom:1px solid #808080;padding-bottom:1pt");
        const QString runCss = rp.css();
        if (!runCss.isEmpty()) css << runCss;

        if (!marker.isEmpty()) {
            // Hanging marker: an inline-block as wide as the hang, so the
            // text starts on the indent like Word's tab-after-number.
            const double hang = first < 0 ? -first : 0;
            inner = QStringLiteral("<span class=\"mk\" style=\"min-width:%1px\">%2</span>")
                        .arg(num(hang), escapeHtml(marker)) + inner;
        }
        if (inner.isEmpty()) inner = QStringLiteral("<br>");

        const int h = headingLevel(styleId, pp);
        const QString tag = h > 0 ? QStringLiteral("h%1").arg(h) : QStringLiteral("p");
        QString html;
        if (pp.pageBreakBefore && *pp.pageBreakBefore) html += QStringLiteral("<div class=\"pb\"></div>");
        html += QStringLiteral("<%1 style=\"%2\">%3</%1>").arg(tag, css.join(QLatin1Char(';')), inner);
        html += after;
        emitted_ += html.size();
        if (emitted_ > kOutputBudget) truncated = true;
        return html;
    }

    QString run(X& x, QString& after) {
        RunProps direct;
        QString content;
        while (x.readNextStartElement()) {
            const auto n = x.name();
            if (content.size() > kInlineBudget) { truncated = true; x.skipCurrentElement(); continue; }
            if (n == QLatin1String("rPr")) { parseRPr(x, direct, pkg_.majorFont, pkg_.minorFont); continue; }
            if (n == QLatin1String("t")) {
                content += escapeHtml(x.readElementText(X::SkipChildElements));
                continue;
            }
            if (n == QLatin1String("drawing") || n == QLatin1String("pict") || n == QLatin1String("object")) {
                content += graphic(x, after);
                continue;
            }
            if (n == QLatin1String("AlternateContent")) {
                // Choice (DrawingML) first; the VML Fallback only when the
                // Choice gave us nothing to show.
                bool shown = false;
                while (x.readNextStartElement()) {
                    const bool choice = x.name() == QLatin1String("Choice");
                    const bool fallback = x.name() == QLatin1String("Fallback");
                    if ((choice || fallback) && !shown) {
                        const qsizetype afterLen = after.size();
                        const QString g = graphic(x, after);
                        shown = !g.isEmpty() || after.size() != afterLen;
                        content += g;
                    } else {
                        x.skipCurrentElement();
                    }
                }
                continue;
            }
            if (n == QLatin1String("tab")) content += QLatin1Char('\t');
            else if (n == QLatin1String("br")) {
                const QString type = attr(x, QLatin1String("type"));
                content += (type == QLatin1String("page") || type == QLatin1String("column"))
                    ? QStringLiteral("<span class=\"pb\"></span>") : QStringLiteral("<br>");
            } else if (n == QLatin1String("cr")) content += QStringLiteral("<br>");
            else if (n == QLatin1String("noBreakHyphen")) content += QChar(0x2011);
            else if (n == QLatin1String("softHyphen")) content += QStringLiteral("&shy;");
            else if (n == QLatin1String("sym")) content += QStringLiteral("•");
            x.skipCurrentElement();   // also fldChar, instrText, delText, refs, …
        }
        RunProps rp;
        if (!direct.styleId.isEmpty()) {
            ParaProps ignored;
            resolveStyle(direct.styleId, ignored, rp);
        }
        rp.mergeFrom(direct);
        if (rp.vanish && *rp.vanish) return {};
        if (content.isEmpty()) return {};
        const QString css = rp.css();
        return css.isEmpty() ? content
                             : QStringLiteral("<span style=\"%1\">%2</span>").arg(css, content);
    }

    // Scans one graphic container (drawing / pict / object / mc:Choice)
    // for pictures and text boxes. Pictures come back inline; text-box
    // content is block-level, so it queues in `after` for the paragraph
    // to emit once it closes.
    QString graphic(X& x, QString& after) {
        QString html, alt;
        double cx = 0, cy = 0;
        bool sized = false;
        int depth = 0;
        while (!x.atEnd()) {
            const auto t = x.readNext();
            if (t == X::StartElement) {
                const auto n = x.name();
                if (n == QLatin1String("txbxContent")) {
                    if (++boxDepth_ <= 4) after += QStringLiteral("<div class=\"txbx\">") + blocks(x) + QStringLiteral("</div>");
                    else { omitted = true; x.skipCurrentElement(); }
                    --boxDepth_;
                    continue;
                }
                if (n == QLatin1String("Fallback")) { x.skipCurrentElement(); continue; }
                if (n == QLatin1String("extent") && cx == 0) {
                    cx = attr(x, QLatin1String("cx")).toDouble() / 9525.0;
                    cy = attr(x, QLatin1String("cy")).toDouble() / 9525.0;
                } else if (n == QLatin1String("docPr")) {
                    alt = attr(x, QLatin1String("descr"));
                } else if (n == QLatin1String("shape") && cx == 0) {
                    static const QRegularExpression w(QStringLiteral("(?:^|;)\\s*width:\\s*([0-9.]+)pt"));
                    const auto m = w.match(attr(x, QLatin1String("style")));
                    if (m.hasMatch()) cx = m.captured(1).toDouble() * 96.0 / 72.0;
                } else if (n == QLatin1String("blip")) {
                    html += image(relAttr(x, QLatin1String("embed")), sized ? 0 : cx, alt);
                    sized = true;
                } else if (n == QLatin1String("imagedata")) {
                    html += image(relAttr(x, QLatin1String("id")), sized ? 0 : cx, alt);
                    sized = true;
                }
                ++depth;
            } else if (t == X::EndElement) {
                if (depth == 0) break;
                --depth;
            }
        }
        Q_UNUSED(cy);
        return html;
    }

    QString image(const QString& relId, double widthPx, const QString& alt) {
        const auto rel = pkg_.rels.constFind(relId);
        if (rel == pkg_.rels.constEnd() || rel->external) return {};   // never fetch remote media
        const QString entry = resolvePart(pkg_.baseDir, rel->target);
        if (entry.isEmpty()) return {};
        const QString ext = QFileInfo(entry).suffix().toLower();
        static const QSet<QString> web = {
            QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
            QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("webp")};
        if (!web.contains(ext))
            return QStringLiteral("<span class=\"noimg\">[%1 image]</span>")
                .arg(escapeHtml(ext.left(8).toUpper()));

        QString src = imageByEntry_.value(entry);
        if (src.isEmpty()) {
            if (images_ >= 400) return {};
            const QByteArray bytes = pkg_.read(entry, 32ULL * 1024 * 1024);
            if (bytes.isEmpty() || bytes.size() > imageBudget_) return {};
            // Staged under OUR name — the entry name never reaches the disk.
            src = QStringLiteral("media/img%1.%2").arg(images_).arg(ext);
            QFile f(stage_ + QLatin1Char('/') + src);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(bytes) != bytes.size())
                return {};
            f.close();
            ++images_;
            imageBudget_ -= bytes.size();
            imageByEntry_.insert(entry, src);
        }
        const QString size = (widthPx >= 1 && widthPx <= 8192)
            ? QStringLiteral(" style=\"width:%1px\"").arg(num(widthPx)) : QString();
        return QStringLiteral("<img src=\"%1\" alt=\"%2\"%3>").arg(src, escapeHtml(alt.left(200)), size);
    }

    void parseTcPr(X& x, Cell& cell) {
        while (x.readNextStartElement()) {
            const auto n = x.name();
            if (n == QLatin1String("gridSpan"))
                cell.span = std::clamp(attr(x, QLatin1String("val")).toInt(), 1, 64);
            else if (n == QLatin1String("vMerge"))
                cell.vmerge = attr(x, QLatin1String("val")) == QLatin1String("restart") ? 1 : 2;
            else if (n == QLatin1String("shd")) cell.fill = hexColor(attr(x, QLatin1String("fill")));
            else if (n == QLatin1String("tcW")) {
                const QString type = attr(x, QLatin1String("type"));
                if (type.isEmpty() || type == QLatin1String("dxa"))
                    cell.width = std::clamp(attr(x, QLatin1String("w")).toDouble(), 0.0, 31680.0);
            } else if (n == QLatin1String("vAlign")) {
                const QString v = attr(x, QLatin1String("val"));
                cell.valign = v == QLatin1String("center") ? QStringLiteral("middle")
                            : v == QLatin1String("bottom") ? QStringLiteral("bottom") : QString();
            }
            x.skipCurrentElement();
        }
    }

    QString table(X& x) {
        if (++tableDepth_ > 6) { omitted = true; x.skipCurrentElement(); --tableDepth_; return {}; }
        std::optional<bool> borders;
        QString styleId, jc;
        std::vector<double> grid;
        std::vector<std::vector<Cell>> rows;
        while (x.readNextStartElement()) {
            const auto n = x.name();
            if (n == QLatin1String("tblPr")) {
                while (x.readNextStartElement()) {
                    const auto c = x.name();
                    if (c == QLatin1String("tblStyle")) styleId = attr(x, QLatin1String("val"));
                    else if (c == QLatin1String("jc")) jc = attr(x, QLatin1String("val"));
                    else if (c == QLatin1String("tblBorders")) {
                        bool any = false;
                        while (x.readNextStartElement()) {
                            if (borderVisible(x)) any = true;
                            x.skipCurrentElement();
                        }
                        borders = any;
                        continue;
                    }
                    x.skipCurrentElement();
                }
                continue;
            }
            if (n == QLatin1String("tblGrid")) {
                while (x.readNextStartElement()) {
                    // (`tableCol` is not OOXML, but minNotes' .docx export
                    // writes it where `gridCol` belongs — read both.)
                    if ((x.name() == QLatin1String("gridCol") || x.name() == QLatin1String("tableCol"))
                        && grid.size() < 256)
                        grid.push_back(std::clamp(attr(x, QLatin1String("w")).toDouble(), 0.0, 31680.0));
                    x.skipCurrentElement();
                }
                continue;
            }
            if (n == QLatin1String("tr")) {
                std::vector<Cell> row;
                int gridCol = 0;
                while (x.readNextStartElement()) {
                    if (x.name() != QLatin1String("tc")) { x.skipCurrentElement(); continue; }
                    if (row.size() >= 256) { omitted = true; x.skipCurrentElement(); continue; }
                    Cell cell;
                    cell.html = blocks(x, &cell);
                    cell.gridCol = gridCol;
                    gridCol += cell.span;
                    row.push_back(std::move(cell));
                }
                if (rows.size() < 20000) rows.push_back(std::move(row));
                else truncated = true;
                continue;
            }
            x.skipCurrentElement();
        }
        --tableDepth_;
        if (rows.empty()) return {};

        // vMerge → rowspan: a restart cell absorbs the `continue` cells
        // stacked under it at the same grid column.
        for (size_t r = 0; r < rows.size(); ++r)
            for (Cell& c : rows[r]) {
                if (c.vmerge != 1) continue;
                for (size_t k = r + 1; k < rows.size(); ++k) {
                    auto it = std::find_if(rows[k].begin(), rows[k].end(), [&](const Cell& o) {
                        return o.gridCol == c.gridCol && o.vmerge == 2 && !o.merged;
                    });
                    if (it == rows[k].end()) break;
                    it->merged = true;
                    ++c.rowspan;
                }
            }

        if (!borders) {   // no direct borders → the table style's (chain)
            QString cur = styleId;
            for (int guard = 0; guard < 16 && !cur.isEmpty() && !borders; ++guard) {
                const auto it = pkg_.styles.constFind(cur);
                if (it == pkg_.styles.constEnd()) break;
                borders = it->tblBorders;
                cur = it->basedOn;
            }
        }

        // No usable grid → the first row whose cells all state a width.
        double total = 0;
        for (double w : grid) total += w;
        if (!(total > 0)) {
            grid.clear();
            for (const auto& row : rows) {
                const bool all = !row.empty() && std::all_of(row.begin(), row.end(),
                    [](const Cell& c) { return c.width > 0; });
                if (!all) continue;
                for (const Cell& c : row)
                    for (int k = 0; k < c.span && grid.size() < 256; ++k)
                        grid.push_back(c.width / c.span);
                break;
            }
            for (double w : grid) total += w;
        }
        // Known widths → fixed layout: the columns hold (an auto-layout
        // cell holding only a max-width:100% image collapses to nothing)
        // and long unbreakable names wrap inside their cell as in Word.
        QString html = QStringLiteral("<table class=\"%1\" style=\"%2%3\">")
            .arg(borders.value_or(false) ? QStringLiteral("t b") : QStringLiteral("t"),
                 total > 0 ? QStringLiteral("table-layout:fixed;width:%1px;").arg(num(total / 15.0)) : QString(),
                 jc == QLatin1String("center") ? QStringLiteral("margin-left:auto;margin-right:auto")
                 : (jc == QLatin1String("right") || jc == QLatin1String("end")) ? QStringLiteral("margin-left:auto")
                 : QString());
        if (total > 0) {
            html += QStringLiteral("<colgroup>");
            for (double w : grid)
                html += QStringLiteral("<col style=\"width:%1%\">").arg(num(w / total * 100.0));
            html += QStringLiteral("</colgroup>");
        }
        for (const auto& row : rows) {
            html += QStringLiteral("<tr>");
            for (const Cell& c : row) {
                if (c.merged) continue;
                QStringList css;
                if (!c.fill.isEmpty()) css << QStringLiteral("background:%1").arg(c.fill);
                if (!c.valign.isEmpty()) css << QStringLiteral("vertical-align:%1").arg(c.valign);
                html += QStringLiteral("<td");
                if (c.span > 1) html += QStringLiteral(" colspan=\"%1\"").arg(c.span);
                if (c.rowspan > 1) html += QStringLiteral(" rowspan=\"%1\"").arg(c.rowspan);
                if (!css.isEmpty()) html += QStringLiteral(" style=\"%1\"").arg(css.join(QLatin1Char(';')));
                html += QLatin1Char('>') + c.html + QStringLiteral("</td>");
            }
            html += QStringLiteral("</tr>");
        }
        return html + QStringLiteral("</table>");
    }
};

// White sheet on the lightbox's dark canvas. %1 page width px, %2–%5
// top/right/bottom/left margins px, %6 body font-family, %7 body size pt.
// white-space:pre-wrap keeps Word's significant spaces and tabs (tab
// stops approximate as Word's default half inch).
const char* kCss =
    "html{background:#181817}"
    "body{background:#181817;margin:0;padding:24px 16px 64px}"
    "main{box-sizing:border-box;width:%1px;max-width:100%;margin:0 auto;"
    "padding:%2px %3px %4px %5px;background:#ffffff;color:#000000;"
    "box-shadow:0 2px 18px rgba(0,0,0,.55);%6;font-size:%7pt;line-height:1.2;"
    "overflow-wrap:break-word}"
    "p,h1,h2,h3,h4,h5,h6{margin:0;white-space:pre-wrap;tab-size:48px;"
    "font-size:inherit;font-weight:inherit}"
    "a{color:#0563c1}"
    "img{max-width:100%;height:auto;vertical-align:bottom}"
    ".mk{display:inline-block;text-indent:0;white-space:pre}"
    ".mk::after{content:' '}"
    ".pb{display:block;height:0;border-top:1px dashed #b5b5b5;margin:28px 0}"
    ".noimg{color:#777;font-size:9pt;border:1px dashed #bbb;padding:2px 6px}"
    ".txbx{border:1px solid #c8c8c8;padding:6px 10px;margin:8px 0}"
    "table.t{border-collapse:collapse;max-width:100%;margin-top:4px;margin-bottom:4px}"
    "table.t td{padding:1px 7px;vertical-align:top;overflow-wrap:anywhere}"
    "table.b td{border:1px solid #000}"
    ".trunc{color:#777;font-size:9pt;margin-top:24px;text-align:center}";

}  // namespace

QString DocxDoc::htmlPreviewPath(const QString& docxPath) const {
    const QFileInfo fi(docxPath);
    if (docxPath.isEmpty() || !fi.isFile()) return {};

    const QString tmpRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString stamp = QStringLiteral("%1:%2:%3")
        .arg(QLatin1String(kRendererRev)).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
    const QString dir = tmpRoot + QStringLiteral("/ufb-docx-%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            docxPath.toUtf8(), QCryptographicHash::Sha1).toHex().left(16)));
    const QString htmlOut = dir + QStringLiteral("/index.html");

    {
        QFile st(dir + QStringLiteral("/.stamp"));
        if (st.open(QIODevice::ReadOnly) && QString::fromUtf8(st.readAll()) == stamp
            && QFileInfo::exists(htmlOut))
            return htmlOut;
    }
    // Rendering for real (not a cache hit): reap week-old stages first —
    // the temp dir is never purged on Windows. Kept off the fast path: it
    // lists the whole temp dir.
    {
        const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-7);
        for (const QFileInfo& d : QDir(tmpRoot).entryInfoList({QStringLiteral("ufb-docx-*")},
                                                              QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (d.absoluteFilePath() == dir) continue;
            const QFileInfo st(d.absoluteFilePath() + QStringLiteral("/.stamp"));
            if (!st.exists() || st.lastModified() < cutoff)
                QDir(d.absoluteFilePath()).removeRecursively();
        }
    }
    QDir(dir).removeRecursively();
    if (!QDir().mkpath(dir + QStringLiteral("/media"))) return {};
    auto fail = [&] { QDir(dir).removeRecursively(); return QString(); };

    Package pkg;
    memset(&pkg.zip, 0, sizeof(pkg.zip));
    if (!mz_zip_reader_init_file(&pkg.zip, docxPath.toUtf8().constData(), 0)) return fail();
    pkg.open = true;

    // Main part via the package relationships; the conventional name is
    // the fallback (and what virtually every writer uses).
    constexpr quint64 kXmlCap = 96ULL * 1024 * 1024;
    QString mainPart = resolvePart(QString(),
        relTargetByType(parseRels(pkg.read(QStringLiteral("_rels/.rels"), kXmlCap)),
                        QLatin1String("/officeDocument")));
    if (mainPart.isEmpty()) mainPart = QStringLiteral("word/document.xml");
    const QByteArray docXml = pkg.read(mainPart, kXmlCap);
    if (docXml.isEmpty()) return fail();

    const qsizetype slash = mainPart.lastIndexOf(QLatin1Char('/'));
    pkg.baseDir = slash > 0 ? mainPart.left(slash) : QString();
    const QString mainName = mainPart.mid(slash + 1);
    pkg.rels = parseRels(pkg.read(
        (pkg.baseDir.isEmpty() ? QString() : pkg.baseDir + QLatin1Char('/'))
            + QStringLiteral("_rels/") + mainName + QStringLiteral(".rels"), kXmlCap));

    auto part = [&](QLatin1String typeSuffix, const QString& conventional) {
        QString p = resolvePart(pkg.baseDir, relTargetByType(pkg.rels, typeSuffix));
        if (p.isEmpty()) p = resolvePart(pkg.baseDir, conventional);
        return p.isEmpty() ? QByteArray() : pkg.read(p, kXmlCap);
    };
    // Theme before styles: rFonts may name a theme font.
    parseTheme(pkg, part(QLatin1String("/theme"), QStringLiteral("theme/theme1.xml")));
    parseStyles(pkg, part(QLatin1String("/styles"), QStringLiteral("styles.xml")));
    parseNumbering(pkg, part(QLatin1String("/numbering"), QStringLiteral("numbering.xml")));

    Renderer r(pkg, dir);
    QString body;
    bool sawBody = false;
    {
        X x(docXml);
        while (!x.atEnd()) {
            if (x.readNext() == X::StartElement && x.name() == QLatin1String("body")) {
                sawBody = true;
                body = r.blocks(x);
                break;
            }
        }
    }
    if (!sawBody) return fail();   // a zip, but not a word document
    if (r.truncated)
        body += QStringLiteral("<div class=\"trunc\">Preview truncated — open the document to see the rest.</div>");
    else if (r.omitted)
        body += QStringLiteral("<div class=\"trunc\">Some deeply nested content is not shown in this preview.</div>");

    const PageGeom& pg = r.page;
    const double pageW = std::clamp(pg.w / 15.0, 320.0, 2400.0);
    auto margin = [&](double twips) { return std::clamp(twips / 15.0, 0.0, pageW * 0.3); };
    const QString bodyFont = pkg.defaultR.font.isEmpty()
        ? QStringLiteral("font-family:Calibri,Carlito,'Helvetica Neue',Arial,sans-serif")
        : fontFamilyCss(pkg.defaultR.font);
    // No docDefaults size → the spec's 10pt.
    const double bodyPt = pkg.defaultR.sizePt > 0 ? pkg.defaultR.sizePt : 10.0;

    const QString html = QStringLiteral("<!doctype html><meta charset=\"utf-8\"><style>%1</style><main>%2</main>")
        .arg(QString::fromLatin1(kCss)
                 .arg(num(pageW), num(margin(pg.top)), num(margin(pg.right)),
                      num(margin(pg.bottom)), num(margin(pg.left)), bodyFont, num(bodyPt)),
             body);

    QFile f(htmlOut);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return fail();
    const QByteArray bytes = html.toUtf8();
    const bool wrote = f.write(bytes) == bytes.size();
    f.close();
    if (!wrote) return fail();   // disk full: never stamp a torn page as fresh
    QFile st(dir + QStringLiteral("/.stamp"));
    if (st.open(QIODevice::WriteOnly | QIODevice::Truncate)) st.write(stamp.toUtf8());
    return htmlOut;
}

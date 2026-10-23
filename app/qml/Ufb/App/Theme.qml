// Theme — central design tokens (colors, dimensions, font handles).
// Singleton so any QML file can do `Theme.colors.accent` /
// `Theme.icon.size.toolbar` without re-importing.
//
// The visual direction follows the Topaz Gigapixel reference: dark,
// flat, blue accent for primary affordances, full-row-height action
// strips — with the family's humanist pass (minNotes' ruling of
// 2026-10-21, QCView 2026-10-23, here 2026-10-23): three corner radii
// applied by surface class, a short hover / pressed colour ramp, and
// popups that fade in. Each value here exists ONCE; refactor
// magic strings ("#4a90e2", "#1a1a1a", padding numbers) elsewhere
// to reference these instead so a future visual sweep stays cheap.
//
// Tokens are grouped:
//   Theme.colors  — surface / text / accent palette
//   Theme.dim     — pixel sizes (heights, paddings, radii, dividers)
//   Theme.icon    — icon sizes + the loaded Phosphor font handle
//   Theme.font    — text font sizes + family

pragma Singleton
import QtQuick
import "PhosphorIcons.js" as PhosphorIcons

QtObject {
    id: theme

    /// image:// URL for a Phosphor icon, for controls that take an
    /// `icon.source` (MenuItem / Action / Button) instead of hosting an
    /// Icon element. Backed by the C++ ufb-glyph provider; tinted by
    /// the control's icon.color. Unknown names yield "" (no icon).
    function glyphUrl(name) {
        var ch = PhosphorIcons.code[name]
        if (!ch) return ""
        return "image://ufb-glyph/" + ch.charCodeAt(0).toString(16)
    }

    // ── Color palette ────────────────────────────────────────────────
    readonly property QtObject colors: QtObject {
        // Surfaces (background → foreground stack).
        readonly property color bg:           "#161616"   // app shell
        readonly property color surface:      "#1a1a1a"   // panels
        readonly property color surfaceAlt:   "#1d1d1d"   // alt rows
        readonly property color surfaceHover: "#252525"   // hover bg
        readonly property color surfacePressed: "#323232" // pressed: one step past hover
        // A ramp's idle end is never "transparent": that is black at
        // alpha 0, so the colour animation would dip dark before it
        // lightens. This is surfaceHover at alpha 0 — the ramp only
        // ever fades in.
        readonly property color hoverIdle:    Qt.rgba(0x25 / 255, 0x25 / 255, 0x25 / 255, 0)
        readonly property color toolbar:      "#1f1f1f"   // header strips
        readonly property color toolbarAlt:   "#262626"   // sub-header

        // Lines.
        readonly property color border:       "#333333"   // panel borders
        readonly property color borderStrong: "#3d3d3d"   // headers / inset
        // Hairline tone for 1px separators — a notch below `border` so
        // the tonal background still carries most of the separation.
        // All three line tones are neutral and one step lighter than
        // the old #222222 / #2a2a2a / #333333, which vanished against
        // the #1a1a1a panels. (A warm-tinted variant was tried and
        // rejected — lines stay neutral; only glyphs carry warmth.)
        readonly property color divider:      "#2a2a2a"   // 1px separators

        // Text.
        readonly property color text:         "#dddddd"   // primary
        readonly property color textMuted:    "#888888"   // secondary
        readonly property color textSubtle:   "#666666"   // tertiary
        readonly property color textBright:   "#ffffff"   // selected/heading
        readonly property color textInverted: "#111111"   // on accent fills

        // Accent + state. Calmer cobalt — chosen so the same color
        // works for primary affordances AND large-area selection
        // backgrounds without needing a separate dimmed variant.
        // The earlier "Topaz" #009aff was too saturated for filled
        // selection rows / panel borders at scale; #3F6BCB reads
        // cleanly as both an action color and a fill.
        readonly property color accent:       "#0189f1"   // primary blue
        readonly property color accentHover:  "#1b95f1"   // slightly lighter
        readonly property color accentMuted:  "#10395b"   // inactive-pane selected-row bg
        // Alias for accent. Kept as a distinct token so the call
        // sites that reach for it (selection backgrounds, tab
        // underlines, panel border) stay self-documenting; if a
        // future visual pass wants to re-dim selections, change
        // this one line.
        readonly property color accentSelected: accent
        // Logo yellow (assets/icons/ufb*.svg). Used sparingly for
        // identity — the sidebar's bookmark / job / tracker glyphs —
        // never for state or actions (that's accent / success / warning).
        readonly property color brand:        "#d9bd57"
        // Sidebar section identities: the brand yellow marks
        // Subscriptions (the jobs); Bookmarks and Trackers share a warm
        // neutral so only the jobs carry colour (green is taken by the
        // mount status dots below). Identity only, not state.
        readonly property color sidebarNeutral:  "#b8b0a4"
        readonly property color sidebarBookmark: sidebarNeutral
        readonly property color sidebarTracker:  sidebarNeutral
        readonly property color success:      "#4cb050"
        readonly property color warning:      "#f5a623"
        readonly property color error:        "#c04040"
        readonly property color info:         "#9cc9ff"
    }

    // ── Dimensions ───────────────────────────────────────────────────
    readonly property QtObject dim: QtObject {
        // Heights.
        readonly property int rowHeight:        24
        readonly property int rowHeightDense:   22
        readonly property int rowHeightTall:    28
        // Two-line row (name + small subtitle) — extra leading so the
        // two labels don't visually collide. Used for Subscriptions /
        // Mounts entries.
        readonly property int rowHeightStacked: 38
        readonly property int toolStripHeight:  32
        readonly property int statusStripHeight:22
        readonly property int headerHeight:     28

        // Paddings.
        readonly property int paddingTight:      4
        readonly property int padding:           8
        readonly property int paddingLoose:     12

        // Spacing between elements.
        readonly property int spacingTight:      2
        readonly property int spacing:           4
        readonly property int spacingLoose:      8

        // Sidebar rhythm: gap between rows inside a section list,
        // breathing room between a section header and its first row,
        // and the space above the hairline that separates sections.
        readonly property int sidebarRowGap:     2
        readonly property int sidebarListPad:    4
        readonly property int sidebarSectionGap: 10
        // Zebra-striped lists (browser list/tree, project item panel)
        // can't take a gap between rows without breaking the stripes,
        // so their rhythm is taller rows (padding inside each stripe)
        // plus this much air under the column header.
        readonly property int listTopPad:        4
        readonly property int listRowHeight:     24   // browser list + tree
        readonly property int listRowHeightSearch: 36 // list row with parent-path subtitle
        readonly property int itemRowHeight:     28   // project item panel
        // Breadcrumb path bar: width of the always-empty, click-to-edit
        // tail on the right that long paths can never cover.
        readonly property int pathBarTail:       48

        // Radii — three, by surface class (the humanist pass). Hairlines,
        // drop lines, progress tracks and glyph circles stay as they are.
        readonly property int radiusSmall:       3    // buttons, segments, chips, pills, wells, fields, menu rows, handles, scrollbar thumbs
        readonly property int radius:            6    // popups, menus, dialogs, toasts, cards, lanes, banners, grid tiles
        readonly property int radiusLarge:      10    // reserved (a notice card, a switch's pill track) — never invent a fourth value

        // Motion.
        readonly property int hoverMs:         100    // the hover / pressed colour ramp
        readonly property int popupMs:          80    // a popup's fade-in. No exit fade: it keeps a
                                                      // modal popup alive for its length and eats the next click.

        // Borders / dividers.
        readonly property int border:            1
        readonly property int divider:           1

        // Resize handle hot-zone width (matches FileBrowser tweak).
        readonly property int handleHotZone:    10

        // Width of UfbScrollBar's thumb. Inner delegates reserve
        // `padding + scrollBarWidth` on their right edge so the bar
        // overlays a gutter rather than text when AsNeeded reveals it.
        readonly property int scrollBarWidth:    8
    }

    // ── Icon font + sizes ────────────────────────────────────────────
    // FontLoaders for the three Phosphor weights we ship: Regular,
    // Fill, and Duotone. All three share the same codepoint table
    // (PhosphorIcons.js); only the family name and rendering style
    // differ. Duotone needs special handling — see Icon.qml — because
    // each glyph is a *pair* (base codepoint = background tone at
    // 0.2 opacity, base+1 = foreground at full opacity).
    readonly property FontLoader phosphorFont: FontLoader {
        source: Qt.resolvedUrl("fonts/Phosphor.ttf")
        onStatusChanged: {
            if (status === FontLoader.Error) {
                console.warn("Theme: failed to load Phosphor.ttf at", source)
            } else if (status === FontLoader.Ready) {
                console.log("Theme: Phosphor font loaded as family:", name)
            }
        }
    }
    readonly property FontLoader phosphorFillFont: FontLoader {
        source: Qt.resolvedUrl("fonts/Phosphor-Fill.ttf")
        onStatusChanged: {
            if (status === FontLoader.Error) {
                console.warn("Theme: failed to load Phosphor-Fill.ttf at", source)
            }
        }
    }
    readonly property FontLoader phosphorDuotoneFont: FontLoader {
        source: Qt.resolvedUrl("fonts/Phosphor-Duotone.ttf")
        onStatusChanged: {
            if (status === FontLoader.Error) {
                console.warn("Theme: failed to load Phosphor-Duotone.ttf at", source)
            }
        }
    }
    readonly property FontLoader phosphorThinFont: FontLoader {
        source: Qt.resolvedUrl("fonts/Phosphor-Thin.ttf")
        onStatusChanged: {
            if (status === FontLoader.Error) {
                console.warn("Theme: failed to load Phosphor-Thin.ttf at", source)
            }
        }
    }
    readonly property QtObject icon: QtObject {
        readonly property string family: theme.phosphorFont.name
        readonly property string familyFill: theme.phosphorFillFont.name
        readonly property string familyDuotone: theme.phosphorDuotoneFont.name
        readonly property string familyThin: theme.phosphorThinFont.name
        readonly property int sizeSmall:    12
        readonly property int sizeToolbar:  16
        readonly property int sizeMedium:   18
        readonly property int sizeLarge:    24
    }

    // ── Text font ────────────────────────────────────────────────────
    // Geist Sans + Geist Mono (Vercel, OFL 1.1; LICENSES/Geist-OFL-1.1.txt),
    // bundled so every OS renders the same chrome (2026-10-23; before this
    // the app fell back to Segoe UI / SF Pro and Consolas / Menlo). Static
    // cuts: Qt files every sans cut under the one family "Geist" and every
    // mono cut under "Geist Mono", so font.bold / font.italic / font.weight
    // pick a real face. main.cpp registers the same files for the
    // application-wide QFont before the engine loads.
    readonly property FontLoader geistFont:           FontLoader { source: Qt.resolvedUrl("fonts/Geist-Regular.ttf") }
    readonly property FontLoader geistBoldFont:       FontLoader { source: Qt.resolvedUrl("fonts/Geist-Bold.ttf") }
    readonly property FontLoader geistItalicFont:     FontLoader { source: Qt.resolvedUrl("fonts/Geist-Italic.ttf") }
    readonly property FontLoader geistBoldItalicFont: FontLoader { source: Qt.resolvedUrl("fonts/Geist-BoldItalic.ttf") }
    readonly property FontLoader geistLightFont:        FontLoader { source: Qt.resolvedUrl("fonts/Geist-Light.ttf") }
    readonly property FontLoader geistLightItalicFont:  FontLoader { source: Qt.resolvedUrl("fonts/Geist-LightItalic.ttf") }
    readonly property FontLoader geistMediumFont:       FontLoader { source: Qt.resolvedUrl("fonts/Geist-Medium.ttf") }
    readonly property FontLoader geistMediumItalicFont: FontLoader { source: Qt.resolvedUrl("fonts/Geist-MediumItalic.ttf") }
    readonly property FontLoader geistSemiBoldFont:     FontLoader { source: Qt.resolvedUrl("fonts/Geist-SemiBold.ttf") }
    readonly property FontLoader geistMonoFont:           FontLoader { source: Qt.resolvedUrl("fonts/GeistMono-Regular.ttf") }
    readonly property FontLoader geistMonoBoldFont:       FontLoader { source: Qt.resolvedUrl("fonts/GeistMono-Bold.ttf") }
    readonly property FontLoader geistMonoItalicFont:     FontLoader { source: Qt.resolvedUrl("fonts/GeistMono-Italic.ttf") }
    readonly property FontLoader geistMonoBoldItalicFont: FontLoader { source: Qt.resolvedUrl("fonts/GeistMono-BoldItalic.ttf") }
    readonly property QtObject font: QtObject {
        // Resolved from the loaders, with the old per-OS names as the
        // fallback while a loader is still in flight.
        readonly property string family: theme.geistFont.status === FontLoader.Ready
                                         ? theme.geistFont.name : "Geist"
        readonly property string mono:   theme.geistMonoFont.status === FontLoader.Ready
                                         ? theme.geistMonoFont.name : "Geist Mono"
        readonly property int sizeTiny:      10
        readonly property int sizeSmall:     11
        readonly property int sizeBody:      12
        readonly property int sizeHeading:   13
    }
}

// Theme — central design tokens (colors, dimensions, font handles).
// Singleton so any QML file can do `Theme.colors.accent` /
// `Theme.icon.size.toolbar` without re-importing.
//
// The visual direction follows the Topaz Gigapixel reference: dark,
// flat, squared corners, blue accent for primary affordances, full-
// row-height action strips. Each value here exists ONCE; refactor
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

        // Radii — squared by default; small radius reserved for pills.
        readonly property int radius:            0
        readonly property int radiusPill:       10

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
    readonly property QtObject font: QtObject {
        readonly property string family:     "Segoe UI"   // Windows; falls back per OS
        // Monospace, resolved per-platform so tabular digits stay fixed-width
        // (otherwise a Windows-only name like "Consolas" silently falls back to
        // a proportional font on macOS and numbers jump as they change).
        readonly property string mono: {
            switch (Qt.platform.os) {
            case "windows": return "Consolas"
            case "osx":     return "Menlo"        // ships with macOS
            default:        return "monospace"
            }
        }
        readonly property int sizeTiny:      10
        readonly property int sizeSmall:     11
        readonly property int sizeBody:      12
        readonly property int sizeHeading:   13
    }
}

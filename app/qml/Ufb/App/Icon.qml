// Icon — render a Phosphor icon by name. Supports three weights:
// Regular (default), Fill, and Duotone. All three share the same
// codepoint table in PhosphorIcons.js.
//
// Usage:
//   Icon { name: "folder" }                                // 16px regular, default text color
//   Icon { name: "gear-six"; size: 14; color: Theme.colors.textMuted }
//   Icon { name: "play"; size: 18; color: Theme.colors.accent; weight: "fill" }
//   Icon { name: "folder-simple"; size: 96; weight: "duotone" }
//
// Names come from the Phosphor regular weight (1512 icons). See
// PhosphorIcons.js for the full map. An unknown name renders as a
// blank space — callers can rely on the slot still occupying its
// usual width.
//
// Duotone implementation: Phosphor's duotone TTF stores each icon as
// a *pair* of consecutive codepoints — even = background tone (drawn
// at 0.2 opacity), odd = foreground (full opacity). PhosphorIcons.js
// holds the even/background codepoint; the foreground is +1. We
// stack two Text elements at the same position to composite the two.

import QtQuick
import "PhosphorIcons.js" as PhosphorIcons

Item {
    id: root

    /// Phosphor icon name. e.g. "folder", "gear-six", "x", "magnifying-glass".
    property string name: ""
    /// Pixel size — default matches Theme.icon.sizeToolbar.
    property int size: 16
    /// Glyph color. Duotone uses this for both the background (at
    /// 0.2 opacity) and foreground passes.
    property color color: Theme.colors.text
    /// Render weight. One of "regular", "fill", "duotone", "thin".
    property string weight: "regular"

    width: size
    height: size

    readonly property string _baseChar: PhosphorIcons.code[name] || ""
    readonly property string _fontFamily: weight === "fill"
        ? Theme.icon.familyFill
        : (weight === "duotone"
            ? Theme.icon.familyDuotone
            : (weight === "thin"
                ? Theme.icon.familyThin
                : Theme.icon.family))

    // Duotone background tone. Same codepoint as the regular glyph,
    // dimmed to 0.2 alpha so the foreground (codepoint+1) reads on top.
    Text {
        anchors.fill: parent
        visible: root.weight === "duotone" && root._baseChar.length > 0
        text: root._baseChar
        font.family: root._fontFamily
        font.pixelSize: root.size
        color: root.color
        opacity: 0.2
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    // Foreground glyph. Regular + Fill render the base codepoint
    // straight; Duotone renders base+1.
    Text {
        anchors.fill: parent
        text: root.weight === "duotone"
            ? (root._baseChar.length > 0
                ? String.fromCharCode(root._baseChar.charCodeAt(0) + 1)
                : "")
            : root._baseChar
        font.family: root._fontFamily
        font.pixelSize: root.size
        color: root.color
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}

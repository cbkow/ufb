// FlatButton — squared, minimal-padding button matching the Topaz
// Gigapixel reference style. Replaces Qt Controls' default Button
// (which renders as a tall pill with ~16px horizontal padding).
//
// Variants:
//   variant: "default"  — transparent fill, hover lights up
//   variant: "primary"  — accent-blue fill (use for the dominant
//                         action in a strip)
//   variant: "danger"   — red fill (destructive)
//
// Use inside ToolStrip for full-row-height segmented look. Use stand-
// alone with explicit `Layout.preferredHeight` for inline buttons.
//
// Icon-only:  FlatButton { iconName: "x"; tooltip: "Close" }
// Icon+text:  FlatButton { iconName: "play"; text: "Start" }
// Text-only:  FlatButton { text: "Save"; variant: "primary" }

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    /// Phosphor icon name (optional). Rendered before `text`.
    property string iconName: ""
    /// Visible label (optional). Empty = icon-only.
    property string text: ""
    /// Tooltip text. Empty = no tooltip.
    property string tooltip: ""
    /// "default" | "primary" | "danger"
    property string variant: "default"
    /// Active/checked appearance — used by SegmentedControl + toggles.
    property bool checked: false
    /// Disable the button (greys + ignores clicks).
    property bool enabled_: true
    /// Icon size. Defaults to Theme.icon.sizeToolbar.
    property int iconSize: Theme.icon.sizeToolbar
    /// Extra horizontal padding inside the row (text breathing room).
    property int padding: 8
    /// Optional icon color override. Empty/transparent = use the
    /// variant's foreground color. Set this when you need to tint
    /// just the glyph (e.g. an accent-blue caret on a transparent
    /// "default" button).
    property color iconColor: "transparent"

    signal clicked()
    /// Emitted on mouse-down / mouse-up. Use for press-and-hold actions
    /// (e.g. the lightbox's fast-seek shuttle); `clicked` still fires on a
    /// normal press+release for tap actions.
    signal pressed()
    signal released()

    // Resolved colors per variant + state.
    // No border at any state — checked-default uses an accentMuted
    // fill so the active/selected segment differentiates by colour
    // alone (matches sister-app FlatButton). Hover stays a tonal
    // surfaceHover swap; primary/danger keep their accent/error
    // fills.
    readonly property color _bgIdle: {
        if (!enabled_) return "transparent"
        if (variant === "primary") return Theme.colors.accent
        if (variant === "danger")  return Theme.colors.error
        return checked ? Theme.colors.accentMuted : "transparent"
    }
    readonly property color _bgHover: {
        if (!enabled_) return _bgIdle
        if (variant === "primary") return Theme.colors.accentHover
        if (variant === "danger")  return Qt.lighter(Theme.colors.error, 1.15)
        return checked ? Theme.colors.accent : Theme.colors.surfaceHover
    }
    readonly property color _fg: {
        if (!enabled_) return Theme.colors.textSubtle
        if (variant === "primary" || variant === "danger") return Theme.colors.textBright
        return checked ? Theme.colors.textBright : Theme.colors.text
    }

    implicitHeight: Theme.dim.toolStripHeight
    implicitWidth: Math.max(
        Theme.dim.toolStripHeight,
        rowContent.implicitWidth + padding * 2)
    radius: Theme.dim.radius
    color: ma.containsMouse ? _bgHover : _bgIdle

    Row {
        id: rowContent
        anchors.centerIn: parent
        spacing: 6

        Icon {
            visible: root.iconName.length > 0
            name: root.iconName
            size: root.iconSize
            color: root.iconColor.a > 0 ? root.iconColor : root._fg
            anchors.verticalCenter: parent.verticalCenter
        }
        Label {
            visible: root.text.length > 0
            text: root.text
            color: root._fg
            font.pixelSize: Theme.font.sizeBody
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: root.enabled_ ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked:  { if (root.enabled_) root.clicked() }
        onPressed:  (mouse) => { if (root.enabled_) root.pressed() }
        onReleased: (mouse) => { if (root.enabled_) root.released() }
        // Treat leaving the button while held as a release, so a held
        // fast-seek can't get stuck on if the cursor slides off.
        onExited:   { if (root.enabled_ && pressed) root.released() }
    }

    ToolTip.text: tooltip
    ToolTip.visible: tooltip.length > 0 && ma.containsMouse
    ToolTip.delay: 500
}

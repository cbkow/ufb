// SegmentedControl — pick-one-of-N flat toggle group, matching the
// Topaz reference's "1x / 2x / 4x / 6x / Custom" row. Single rounded
// outline; active segment fills with the surface-hover tone (or the
// accent if `variant: "accent"`).
//
// Usage:
//   SegmentedControl {
//       options: [
//           { value: "list", icon: "list" },
//           { value: "grid", icon: "squares-four" },
//           { value: "tree", icon: "tree-view" }
//       ]
//       value: "list"
//       onValueChanged: panel.viewMode = value
//   }

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    /// Array of { value, label?, icon?, tooltip? }. label OR icon
    /// (or both) — value is the unique identifier the binding sees.
    property var options: []
    /// Currently-selected value. Two-way: assignment from outside
    /// updates the visual; user click emits valueChanged.
    property string value: ""
    /// "default" (border + filled active) | "accent" (active = blue)
    property string variant: "default"

    implicitHeight: Theme.dim.toolStripHeight
    implicitWidth: row.implicitWidth
    // No outer border — active-segment fill alone signals which option
    // is selected. Matches the borderless FlatButton idiom: the row of
    // toggles reads as adjacent cells, with the active one filled.
    color: "transparent"
    radius: Theme.dim.radius

    Row {
        id: row
        anchors.fill: parent
        spacing: 0
        Repeater {
            model: root.options
            delegate: Rectangle {
                required property int index
                required property var modelData
                readonly property bool active: root.value === modelData.value
                width: Math.max(Theme.dim.toolStripHeight,
                    seg.implicitWidth + 16)
                height: parent.height
                color: active
                    ? (root.variant === "accent"
                        ? Theme.colors.accent
                        : Theme.colors.surfaceHover)
                    : (segMa.containsMouse
                        ? Theme.colors.surface
                        : "transparent")

                Row {
                    id: seg
                    anchors.centerIn: parent
                    spacing: 4
                    Icon {
                        visible: !!modelData.icon
                        name: modelData.icon || ""
                        size: Theme.icon.sizeToolbar
                        color: parent.parent.active && root.variant === "accent"
                            ? Theme.colors.textBright
                            : Theme.colors.text
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Label {
                        visible: !!modelData.label
                        text: modelData.label || ""
                        color: parent.parent.active && root.variant === "accent"
                            ? Theme.colors.textBright
                            : Theme.colors.text
                        font.pixelSize: Theme.font.sizeBody
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                MouseArea {
                    id: segMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.value = modelData.value
                }

                ToolTip.text: modelData.tooltip || ""
                ToolTip.visible: !!modelData.tooltip && segMa.containsMouse
                ToolTip.delay: 500
            }
        }
    }
}

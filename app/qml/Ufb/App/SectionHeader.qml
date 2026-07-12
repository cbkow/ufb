// SectionHeader — small caps grey label that introduces a sidebar /
// dialog section. Optional trailing slot (gear, count) that's
// right-aligned to the row.
//
// Usage:
//   SectionHeader { text: "MOUNTS" }
//   SectionHeader {
//       text: "MOUNTS"
//       trailing: FlatButton { iconName: "gear-six"; tooltip: "Settings" }
//   }

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string text: ""
    /// Optional trailing item (icon button, count badge). Pinned to
    /// the right edge of the row.
    default property alias trailing: trailingSlot.children

    implicitHeight: 18

    Label {
        anchors.left: parent.left
        anchors.leftMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        text: root.text
        color: Theme.colors.textSubtle
        font.pixelSize: Theme.font.sizeTiny
        font.bold: true
    }
    Row {
        id: trailingSlot
        anchors.right: parent.right
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4
    }
}

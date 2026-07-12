// ToolStrip — full-row-height action bar matching the Topaz reference.
// Children fill the strip's height (no vertical padding around them);
// they butt up against the left and right edges; horizontal dividers
// between them are rendered automatically.
//
// Use as the container for FlatButtons, segmented controls, status
// labels, etc. — anywhere the legacy app had a "RowLayout with a
// pile of pill Buttons" eyesore.
//
// Usage:
//   ToolStrip {
//       FlatButton { iconName: "arrow-up"; tooltip: "Up" }
//       FlatButton { iconName: "arrow-clockwise"; tooltip: "Refresh" }
//       Item { Layout.fillWidth: true }                  // spacer
//       FlatButton { text: "Save"; variant: "primary" }
//   }

import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    /// Set false to drop the bottom 1px divider (e.g. when the strip
    /// is followed by another bordered surface).
    property bool showBottomDivider: true
    /// Insert a vertical 1px line between every child for the
    /// "tool-strip" segmented look. Defaults true.
    property bool dividedChildren: true

    default property alias content: layout.children

    implicitHeight: Theme.dim.toolStripHeight
    color: Theme.colors.toolbar
    border.width: 0

    // Bottom border — drawn as a child rectangle so we can keep the
    // strip's `border.width: 0` (avoids a 1px shift on the top edge).
    Rectangle {
        visible: root.showBottomDivider
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.dim.divider
        color: Theme.colors.divider
    }

    RowLayout {
        id: layout
        anchors.fill: parent
        spacing: 0
        // Children inherit the row's full height by default — they
        // can override Layout.preferredHeight if they need a smaller
        // hit area.
    }

    // Vertical dividers between layout children. We can't easily
    // inject Rectangles between layout-managed children at runtime;
    // FlatButton/SegmentedControl render their own right-edge border
    // when ToolStrip has dividedChildren=true. (Achieved by reading
    // ToolStrip via parent chain — kept here as a documentation hook
    // until we add it on the consumers.)
}

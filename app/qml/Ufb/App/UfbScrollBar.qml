// UfbScrollBar — themed ScrollBar with a flat thumb, transparent
// track, and policy-honouring visibility. Derived from
// QtQuick.Templates.ScrollBar (not Basic/Fusion) so we control the
// visible/hover/opacity behaviour ourselves — the stock styles
// either fade the thumb to near-invisible on dark backgrounds or
// pull colour from a QML palette QApplication::setPalette can't
// reach.
//
// Usage mirrors plain ScrollBar:
//   ListView { ScrollBar.vertical: UfbScrollBar {} }
//   ScrollView { ScrollBar.vertical: UfbScrollBar { policy: ScrollBar.AlwaysOn } }
//
// Default policy is AsNeeded, so the bar only appears when content
// overflows. Inner list/grid delegates should reserve a right gutter
// of `Theme.dim.padding + Theme.dim.scrollBarWidth` so the thumb sits
// in empty space rather than over text.

import QtQuick
import QtQuick.Templates as T

T.ScrollBar {
    id: root

    policy: T.ScrollBar.AsNeeded

    // Templates ScrollBar defaults hoverEnabled to false on platforms
    // where Qt.styleHints.useHoverEffects is false (notably macOS with
    // a trackpad). Force it on so the thumb can react to hover.
    hoverEnabled: true

    // Templates ScrollBar doesn't enforce policy visibility — Basic/
    // Fusion do that via opacity transitions. Bind visible directly so
    // AsNeeded actually hides the bar when content fits, and AlwaysOff
    // hides it unconditionally.
    visible: policy === T.ScrollBar.AlwaysOn
          || (policy === T.ScrollBar.AsNeeded && size < 1.0)

    // Fixed thickness regardless of orientation. Locking it keeps
    // adjacent content from reflowing when the bar appears/disappears.
    implicitWidth:  orientation === Qt.Vertical   ? Theme.dim.scrollBarWidth : 0
    implicitHeight: orientation === Qt.Horizontal ? Theme.dim.scrollBarWidth : 0

    // Transparent track — the thumb alone signals scroll position.
    background: Rectangle {
        color: "transparent"
    }

    contentItem: Rectangle {
        implicitWidth:  root.orientation === Qt.Vertical   ? Theme.dim.scrollBarWidth : 0
        implicitHeight: root.orientation === Qt.Horizontal ? Theme.dim.scrollBarWidth : 0
        radius: Theme.dim.radius
        // Hover is tracked on the thumb itself rather than via
        // root.hovered: the control's hover area covers the full
        // (often transparent) track, so a HoverHandler on the thumb
        // brightens only what the cursor actually sits on.
        color: root.pressed ? Theme.colors.textMuted
             : thumbHover.hovered ? Theme.colors.textSubtle
                                  : Theme.colors.borderStrong

        HoverHandler {
            id: thumbHover
            cursorShape: Qt.ArrowCursor
        }
    }
}

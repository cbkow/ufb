// FlatSlider — slim flat slider matching the sister app (QCView): a 2px
// track in borderStrong, a bright fill from the origin to the handle, and a
// 10×10 softly rounded handle (the humanist pass). The track stays square.
//
// Usage:
//   FlatSlider { from: 0; to: 1; value: 0.5; onMoved: ... }
//   FlatSlider { accent: true; ... }   // accent fill + handle (the browser's grid-size slider)

import QtQuick
import QtQuick.Controls.Basic

Slider {
    id: root

    /// Accent-coloured fill and handle instead of the neutral bright ones —
    /// for a slider that sets a view parameter rather than a transport
    /// position.
    property bool accent: false

    background: Rectangle {
        x: root.leftPadding
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width:  root.availableWidth
        height: 2
        color:  Theme.colors.borderStrong
        Rectangle {
            width:  root.visualPosition * parent.width
            height: parent.height
            // Bright (or accent) fill while enabled; dims to muted when
            // disabled (e.g. the volume slider with no audio track).
            color:  !root.enabled ? Theme.colors.textSubtle
                  : root.accent   ? Theme.colors.accent : Theme.colors.textBright
        }
    }
    handle: Rectangle {
        x: root.leftPadding + root.visualPosition * (root.availableWidth - width)
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: 10; height: 10
        radius: Theme.dim.radiusSmall
        color: !root.enabled ? Theme.colors.textSubtle
             : root.accent    ? (root.pressed ? Theme.colors.accentHover : Theme.colors.accent)
                              : (root.pressed ? Theme.colors.text : Theme.colors.textBright)
        Behavior on color { ColorAnimation { duration: Theme.dim.hoverMs } }
    }
    opacity: root.enabled ? 1.0 : 0.55
}

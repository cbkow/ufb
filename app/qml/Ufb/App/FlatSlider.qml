// FlatSlider — slim flat slider matching the sister app (QCView): a 2px
// track in borderStrong, a bright fill from the origin to the handle, and a
// 10×10 squared handle. Squared corners, no animation, instant color swaps.
//
// Usage:
//   FlatSlider { from: 0; to: 1; value: 0.5; onMoved: ... }

import QtQuick
import QtQuick.Controls.Basic

Slider {
    id: root

    background: Rectangle {
        x: root.leftPadding
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width:  root.availableWidth
        height: 2
        color:  Theme.colors.borderStrong
        Rectangle {
            width:  root.visualPosition * parent.width
            height: parent.height
            // Bright fill while enabled; dims to muted when disabled
            // (e.g. the volume slider with no audio track).
            color:  root.enabled ? Theme.colors.textBright : Theme.colors.textSubtle
        }
    }
    handle: Rectangle {
        x: root.leftPadding + root.visualPosition * (root.availableWidth - width)
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: 10; height: 10
        radius: Theme.dim.radius
        color: !root.enabled
                 ? Theme.colors.textSubtle
                 : (root.pressed ? Theme.colors.text : Theme.colors.textBright)
    }
    opacity: root.enabled ? 1.0 : 0.55
}

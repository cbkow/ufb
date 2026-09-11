// UfbMenuItem — MenuItem with a Phosphor icon by name. Use inside
// UfbMenu for every context-menu entry so the whole menu carries the
// same leading icon column (an icon-less item in an iconed menu sits
// misaligned). Works with `action:` too — the iconName set here wins
// over the Action's (empty) icon.
//
//   UfbMenuItem { iconName: "copy"; action: copyAction }
//   UfbMenuItem { iconName: "file-zip"; text: qsTr("Compress to ZIP"); onTriggered: ... }
//
// Rendering: the FluentWinUI3 MenuItem's IconLabel takes icon.source
// and tints it with icon.color, so the glyph is an image from the
// ufb-glyph provider (Theme.glyphUrl), not a Text element.

import QtQuick
import QtQuick.Controls

MenuItem {
    /// Phosphor icon name (see PhosphorIcons.js). "" = no icon.
    property string iconName: ""

    icon.source: iconName.length > 0 ? Theme.glyphUrl(iconName) : ""
    icon.color: enabled ? Theme.colors.textMuted : Theme.colors.textSubtle
    icon.width: 14
    icon.height: 14
}

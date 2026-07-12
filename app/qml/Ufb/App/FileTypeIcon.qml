import QtQuick
import "FileExtensionIcons.js" as FileIcons

// OS-native file-type icon.
//
// Renders the Windows Shell / macOS NSWorkspace icon for `extension`
// (or the OS folder icon when `isDir`), served asynchronously by the
// image://ufb-icons/ provider. Used by every file-browser view in
// place of the old Phosphor category glyph.
//
// If the OS icon fails to resolve, falls back to a Phosphor category
// glyph so the view never shows an empty slot. That fallback is the
// only remaining consumer of FileExtensionIcons.js.
//
// OS icons are full-color raster and cannot be tinted — `fallbackColor`
// applies to the Phosphor fallback glyph only.
Item {
    id: root

    property bool isDir: false
    property string extension: ""

    // Pixel size the OS icon texture is decoded / cached at. The
    // provider always extracts 256px; Qt downscales (mipmapped) to
    // this size. Callers pass a fixed value so a resize doesn't
    // re-decode every frame; defaults to the layout size.
    property int sourceSizePx: Math.max(1, Math.round(Math.max(width, height)))

    // Tint for the Phosphor fallback glyph only (OS icons stay
    // full-color regardless).
    property color fallbackColor: Theme.colors.text

    readonly property string _key: isDir
        ? "folder"
        : (extension && extension.length ? extension.toLowerCase() : "file")

    Image {
        id: osIcon
        anchors.fill: parent
        asynchronous: true
        cache: true
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true          // load-bearing for the 256px -> 16px list-view downscale
        sourceSize.width: root.sourceSizePx
        sourceSize.height: root.sourceSizePx
        source: "image://ufb-icons/" + root._key
        visible: status === Image.Ready
    }

    // Phosphor fallback — instantiated if the OS icon fails to resolve.
    // The provider reports a null icon as an error (status Error), so this
    // reliably catches unresolved types and never leaves a blank slot.
    // (Gating on status, not sourceSize/paintedWidth: those reflect the
    // explicit sourceSize request, not whether real pixels arrived.)
    Loader {
        anchors.fill: parent
        active: osIcon.status === Image.Error
        visible: active
        sourceComponent: Icon {
            anchors.centerIn: parent
            name: root.isDir
                ? "folder-simple"
                : FileIcons.glyphForExtension(root.extension)
            weight: root.isDir ? "duotone" : "fill"
            size: Math.min(root.width, root.height)
            color: root.isDir
                ? root.fallbackColor
                : FileIcons.colorForExtension(root.extension, root.fallbackColor)
        }
    }
}

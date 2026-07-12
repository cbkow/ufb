.pragma library

// Map a file extension (without dot) to a Phosphor icon name and a
// category color. Anything not in the map falls back to "file" + the
// caller-provided fallback color.
//
// FALLBACK ONLY. File-type icons now come from the OS shell via the
// image://ufb-icons/ provider (see FileTypeIcon.qml). This map is
// consulted solely when that OS icon fails to resolve, so the views
// never show an empty slot. It is no longer the primary icon source.
//
// Two-step lookup: extension → category → {glyph, color}. Categories
// keep the color palette tight (one color per family of file types)
// rather than per-extension. Tweak palette in `styles` and it
// propagates to every extension in that category.
//
// Glyph values must be standalone Phosphor names — comma-aliased keys
// in PhosphorIcons.js (e.g. "folder, folder-notch") won't resolve.
// Verify new entries against PhosphorIcons.js before adding.

var category = {
    // Raster / camera / DCC
    "jpg":"image", "jpeg":"image", "png":"image", "gif":"image",
    "bmp":"image", "webp":"image", "tif":"image", "tiff":"image",
    "heic":"image", "heif":"image",
    "exr":"image", "hdr":"image", "dpx":"image",
    "psd":"image", "psb":"image", "ai":"image",
    "raw":"image", "cr2":"image", "cr3":"image",
    "nef":"image", "arw":"image", "dng":"image",
    "svg":"svg",

    // Video
    "mp4":"video", "m4v":"video",
    "mov":"video", "avi":"video", "mkv":"video",
    "webm":"video", "flv":"video", "wmv":"video",
    "mpg":"video", "mpeg":"video",
    "mxf":"video", "r3d":"video", "braw":"video",
    "ari":"video", "arx":"video",

    // Audio
    "wav":"audio", "mp3":"audio", "flac":"audio",
    "aac":"audio", "ogg":"audio", "m4a":"audio",
    "wma":"audio", "aif":"audio", "aiff":"audio",
    "opus":"audio",

    // Archives
    "zip":"zip",
    "rar":"archive", "7z":"archive",
    "tar":"archive", "gz":"archive", "tgz":"archive",
    "bz2":"archive", "xz":"archive",

    // Office / docs
    "pdf":"pdf",
    "doc":"doc", "docx":"doc",
    "xls":"xls", "xlsx":"xls",
    "ppt":"ppt", "pptx":"ppt",
    "csv":"csv",

    // Text
    "txt":"text",
    "md":"md", "markdown":"md",
    "rtf":"text", "log":"text",
    "ini":"text", "cfg":"text", "conf":"text",

    // Data / config
    "json":"code", "jsonc":"code",
    "yaml":"code", "yml":"code",
    "xml":"code", "toml":"code", "env":"code",

    // Web / markup
    "html":"html", "htm":"html",
    "css":"css", "scss":"css", "less":"css", "sass":"css",

    // Scripts / source
    "js":"js", "mjs":"js", "cjs":"js",
    "ts":"ts", "tsx":"tsx", "jsx":"jsx",
    "vue":"vue",
    "py":"py", "rs":"rs",
    "c":"c", "h":"c",
    "cpp":"cpp", "cxx":"cpp", "cc":"cpp", "hpp":"cpp",
    "cs":"cs",
    "sql":"sql",

    // Crypto / signed
    "asc":"lock", "gpg":"lock", "pgp":"lock",
    "key":"lock", "pem":"lock"
};

// Category palette. Picked for legibility on the dark Theme surface
// (#161616 → #2a2a2a) AND against the accent-blue selection fill
// (#0189f1) — i.e. icon stays distinguishable in both states.
var styles = {
    "image":   { glyph: "file-image",   color: "#ff8a65" }, // coral
    "svg":     { glyph: "file-svg",     color: "#ffb74d" }, // amber
    "video":   { glyph: "file-video",   color: "#4dd0e1" }, // cyan
    "audio":   { glyph: "file-audio",   color: "#b388ff" }, // violet

    "zip":     { glyph: "file-zip",     color: "#ffb74d" },
    "archive": { glyph: "file-archive", color: "#ffb74d" },

    "pdf":     { glyph: "file-pdf",     color: "#ef5350" }, // red
    "doc":     { glyph: "file-doc",     color: "#64b5f6" }, // word-blue
    "xls":     { glyph: "file-xls",     color: "#81c784" }, // excel-green
    "ppt":     { glyph: "file-ppt",     color: "#ff8a65" }, // ppt-orange
    "csv":     { glyph: "file-csv",     color: "#81c784" },

    "text":    { glyph: "file-text",    color: "#cccccc" },
    "md":      { glyph: "file-md",      color: "#cccccc" },

    "code":    { glyph: "file-code",    color: "#80cbc4" }, // teal-mint
    "html":    { glyph: "file-html",    color: "#ff7043" },
    "css":     { glyph: "file-css",     color: "#42a5f5" },
    "js":      { glyph: "file-js",      color: "#ffd54f" },
    "ts":      { glyph: "file-ts",      color: "#42a5f5" },
    "tsx":     { glyph: "file-tsx",     color: "#42a5f5" },
    "jsx":     { glyph: "file-jsx",     color: "#42a5f5" },
    "vue":     { glyph: "file-vue",     color: "#81c784" },
    "py":      { glyph: "file-py",      color: "#ffd54f" },
    "rs":      { glyph: "file-rs",      color: "#ff8a65" },
    "c":       { glyph: "file-c",       color: "#9fa8da" },
    "cpp":     { glyph: "file-cpp",     color: "#9fa8da" },
    "cs":      { glyph: "file-c-sharp", color: "#b388ff" },
    "sql":     { glyph: "file-sql",     color: "#80cbc4" },

    "lock":    { glyph: "file-lock",    color: "#e57373" }
};

var DEFAULT = { glyph: "file", color: "" };

function styleForExtension(ext) {
    if (!ext) return DEFAULT;
    var key = ("" + ext).toLowerCase();
    var cat = category[key];
    if (cat && styles[cat]) return styles[cat];
    return DEFAULT;
}

function glyphForExtension(ext) {
    return styleForExtension(ext).glyph;
}

// Caller supplies `fallback` so unmatched extensions inherit the row
// text color (selected vs. normal); matched extensions use the
// category color regardless of selection state, since the whole
// point of the color is to scan the type at a glance.
function colorForExtension(ext, fallback) {
    var c = styleForExtension(ext).color;
    return c ? c : fallback;
}

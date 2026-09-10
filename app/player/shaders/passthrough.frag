#version 440

// Passthrough fragment shader — samples an RGBA texture for the lightbox's
// CPU video path (software-decoded / scrubbed frames uploaded from a QImage)
// and, on Windows, the Vulkan→D3D11 bridge's RGBA output.
// Flips Y to match the YUV shaders: the fullscreen triangle maps clip-space
// y=+1 (top) to v_uv.y=1, but the uploaded image's row 0 is the top, so an
// unflipped sample renders upside down. The Metal YUV shaders flip the same
// way, keeping CPU and zero-copy frames consistently oriented.
//
// Rotation: the renderer fits the viewport to the DISPLAY size (swapped
// axes for odd quarter-turns); rotatedSrcUv inverse-rotates the normalized
// display position back onto the stored (unrotated) texture. Same math as
// QCView's compositors; `p` is top-left-origin, i.e. the Y-flipped v_uv.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Uniforms {
    int   matrixIdx;   // unused here (YUV shaders only)
    int   fullRange;   // unused here
    int   rotQ;        // 0/1/2/3 = 0/90/180/270 degrees clockwise
    int   pad1;
} u;

layout(binding = 1) uniform sampler2D u_tex;

vec2 rotatedSrcUv(vec2 p) {
    if (u.rotQ == 1) return vec2(p.y, 1.0 - p.x);          //  90 CW
    if (u.rotQ == 2) return vec2(1.0 - p.x, 1.0 - p.y);    // 180
    if (u.rotQ == 3) return vec2(1.0 - p.y, p.x);          // 270 CW
    return p;
}

void main() {
    fragColor = texture(u_tex, rotatedSrcUv(vec2(v_uv.x, 1.0 - v_uv.y)));
}

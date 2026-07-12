#version 440

// Passthrough fragment shader — samples an RGBA texture for the lightbox's
// CPU video path (software-decoded / scrubbed frames uploaded from a QImage).
// Flips Y to match the YUV shaders: the fullscreen triangle maps clip-space
// y=+1 (top) to v_uv.y=1, but the uploaded image's row 0 is the top, so an
// unflipped sample renders upside down. The Metal YUV shaders flip the same
// way, keeping CPU and zero-copy frames consistently oriented.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D u_tex;

void main() {
    fragColor = texture(u_tex, vec2(v_uv.x, 1.0 - v_uv.y));
}

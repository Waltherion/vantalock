#version 440

// Positioned textured quad (NDC position + uv from a vertex buffer). Used for the
// clock/date (and later the password field) drawn on top of the lock background.

layout(location = 0) in vec2 pos; // NDC
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 v_uv;

void main()
{
    v_uv = uv;
    gl_Position = vec4(pos, 0.0, 1.0);
}

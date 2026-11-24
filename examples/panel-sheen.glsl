#version 330
in vec2 texcoord;
uniform sampler2D tex;
vec4 default_post_processing(vec4 c);

vec4 window_shader() {
    vec2 uv = texcoord / vec2(textureSize(tex, 0));
    vec4 c = texture(tex, uv);
    // A faint, palette-neutral highlight across the upper edge of the panel.
    float sheen = (1.0 - smoothstep(0.04, 0.34, uv.y)) * 0.045 * smoothstep(0.8, 1.0, c.a);
    c.rgb = mix(c.rgb, vec3(c.a), sheen);
    return default_post_processing(c);
}

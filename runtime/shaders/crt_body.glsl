// CRT filter, shared by present.cpp (OpenGL, through crt_body.glsl.h) and crt.frag (Vulkan).
// The includer defines: SRC (sampler2D), RECT (image x, y, w, h in texels, y = top row),
// TEX_SIZE (texels), OUT_SIZE (pixels), LINES (PS1 lines), TIME (seconds).
// One pass after RetroArch's newpixie-crt (bleed, ghosting, rolling scanlines, filmic curve,
// noise, flicker; no frame accumulation pass), on a curved screen with rounded corners.

// Bilinear inside the game picture only: the texels around it are other VRAM (an edge sampled
// across the border picked up their colour, a red line along the right edge).
vec3 crt_fetch(vec2 p) {
    vec2 t = clamp(RECT.xy + p * RECT.zw, RECT.xy + 0.5, RECT.xy + RECT.zw - 0.5);
    return texture(SRC, t / TEX_SIZE).rgb;
}
vec3 crt_pow(vec3 c, float g) { return pow(max(c, vec3(0.0)), vec3(g)); }
float crt_luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// Barrel curvature: 0..1 screen position -> 0..1 image position.
vec2 crt_warp(vec2 uv, vec2 k) {
    vec2 c = uv * 2.0 - 1.0;
    c *= 1.0 + vec2(c.y * c.y, c.x * c.x) * k;
    return c * 0.5 + 0.5;
}

// 1 inside the tube face, 0 outside, rounded corners of `radius` (a fraction of the height)
// with a one-pixel soft edge.
float crt_corners(vec2 p, float radius) {
    vec2 px = p * OUT_SIZE, mid = OUT_SIZE * 0.5;
    float r = radius * OUT_SIZE.y;
    vec2 q = abs(px - mid) - (mid - r);
    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
    return clamp(0.5 - d, 0.0, 1.0);
}

// newpixie-crt
float crt_rand(vec2 co) { return fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453); }
vec3 crt_px(vec2 tc) { return crt_pow(crt_fetch(tc), 2.2) * 1.25; }
vec3 crt_blur(vec2 tc) {  // stands in for newpixie's blur passes
    vec2 b = vec2(2.0 / (LINES * 4.0 / 3.0), 2.0 / LINES);
    return (crt_px(tc) * 2.0 + crt_px(tc + vec2(b.x, 0.0)) + crt_px(tc - vec2(b.x, 0.0)) + crt_px(tc + vec2(0.0, b.y)) +
            crt_px(tc - vec2(0.0, b.y))) / 6.0;
}
vec3 crt_filmic(vec3 c) {
    vec3 x = max(vec3(0.0), c - vec3(0.004));
    return (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06);
}

vec3 crt_pixie(vec2 p, vec2 px, float column) {
    float time = TIME;
    float x = sin(px.y * 1.5) / OUT_SIZE.x * 0.25;
    vec3 col;
    col.r = crt_px(vec2(x + p.x + 0.0009, p.y + 0.0009)).r + 0.02;
    col.g = crt_px(vec2(x + p.x + 0.0000, p.y - 0.0011)).g + 0.02;
    col.b = crt_px(vec2(x + p.x - 0.0015, p.y + 0.0000)).b + 0.02;
    float i = clamp(crt_luma(col), 0.0, 1.0);
    i = 1.0 - i * i;
    i = (1.0 - i) * 0.85 + 0.15;

    // ghosting: faint offset copies of the blurred picture
    float ghs = 0.15;
    vec2 wob1 = 0.007 * vec2(0.35 * sin(1.0 / 7.0 + 15.0 * p.y + 0.9 * time), 0.35 * sin(2.0 / 7.0 + 10.0 * p.y + 1.37 * time));
    vec2 wob2 = 0.007 * vec2(0.35 * sin(2.0 / 7.0 + 15.0 * p.y + 0.9 * time), 0.35 * sin(1.0 / 7.0 + 10.0 * p.y + 1.37 * time));
    vec2 wob3 = 0.007 * vec2(0.35 * sin(1.0 / 7.0 + 15.0 * p.y + 0.9 * time), 0.35 * sin(3.0 / 7.0 + 10.0 * p.y + 1.37 * time));
    vec3 r = crt_blur(vec2(x - 0.014, -0.027) * 0.85 + wob1 + p + 0.001) * vec3(0.5, 0.25, 0.25);
    vec3 g = crt_blur(vec2(x - 0.019, -0.020) * 0.85 + wob2 + p - 0.002) * vec3(0.25, 0.5, 0.25);
    vec3 b = crt_blur(vec2(x - 0.017, -0.003) * 0.85 + wob3 + p - 0.002) * vec3(0.25, 0.25, 0.5);
    col += vec3(ghs * (1.0 - 0.299)) * crt_pow(clamp(3.0 * r, 0.0, 1.0), 2.0) * i;
    col += vec3(ghs * (1.0 - 0.587)) * crt_pow(clamp(3.0 * g, 0.0, 1.0), 2.0) * i;
    col += vec3(ghs * (1.0 - 0.114)) * crt_pow(clamp(3.0 * b, 0.0, 1.0), 2.0) * i;

    col *= vec3(0.95, 1.05, 0.95);
    col = clamp(col * 1.3 + 0.75 * col * col + 1.25 * col * col * col * col * col, vec3(0.0), vec3(10.0));
    float vig = 0.01 + 16.0 * p.x * p.y * (1.0 - p.x) * (1.0 - p.y);
    col *= max(1.3 * sqrt(max(vig, 0.0)), 0.35);

    // rolling scanlines, one per PS1 line
    float scans = clamp(0.35 + 0.18 * sin(6.0 * time - p.y * LINES * 6.2831853), 0.0, 1.0);
    col *= pow(scans, 0.9) * 2.2;  // (brighter than newpixie: this game is dark)
    col *= 1.0 - 0.23 * clamp(mod(column, 3.0) / 2.0, 0.0, 1.0);  // aperture lines
    col = crt_filmic(col);
    vec2 seed = p * OUT_SIZE;
    col -= 0.015 * crt_pow(vec3(crt_rand(seed + time), crt_rand(seed + time * 2.0), crt_rand(seed + time * 3.0)), 1.5);
    col *= 1.0 - 0.004 * (sin(50.0 * time + p.y * 2.0) * 0.5 + 0.5);
    return clamp(col, 0.0, 1.0);
}

// `column`: the output pixel's own column (gl_FragCoord.x). The aperture pattern must follow real
// pixels: uv * OUT_SIZE drifts from them when the picture's width is not whole, and the pattern
// then jumped by a pixel somewhere across the screen (a vertical line).
vec4 crt(vec2 uv, float column) {
    vec2 p = crt_warp(uv, vec2(0.05, 0.065));
    float face = crt_corners(p, 0.06);
    if (face <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);
    vec3 col = crt_pixie(p, uv * OUT_SIZE, floor(column));
    return vec4(col * face, 1.0);
}

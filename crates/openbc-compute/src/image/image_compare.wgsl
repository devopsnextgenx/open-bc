// Bit-exact GPU mirror of openbc_core::image_compare::PixelRenderer.
// Integer math only. Pixels are packed RGBA8 little-endian (R in the low byte),
// i.e. the raw RgbaImage bytes uploaded as array<u32>.
//
// stats buffer layout (u32 x 7):
//   [0] compared  [1] identical  [2] similar  [3] different
//   [4] sum_delta low word  [5] sum_delta high word (carry)  [6] max_delta
// The host must zero `stats` before dispatch and dispatch
// ceil(canvas_w/16) x ceil(canvas_h/16) workgroups.

struct Params {
    canvas_w: u32, canvas_h: u32, right_w: u32, right_h: u32,
    offset_x: i32, offset_y: i32,
    mode: u32,          // 0 tolerance, 1 range, 2 blend
    tolerance: u32,
    range_min: u32, range_max: u32,
    weight256: u32,
    compare_alpha: u32,
    pad0: u32, pad1: u32, pad2: u32, pad3: u32,
};

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> left_px: array<u32>;
@group(0) @binding(2) var<storage, read> right_px: array<u32>;
@group(0) @binding(3) var<storage, read_write> out_px: array<u32>;
@group(0) @binding(4) var<storage, read_write> stats: array<atomic<u32>, 7>;

// compared, identical, similar, different, sum, max
var<workgroup> wg: array<atomic<u32>, 6>;

fn unpack(v: u32) -> vec4<u32> {
    return vec4<u32>(v & 255u, (v >> 8u) & 255u, (v >> 16u) & 255u, (v >> 24u) & 255u);
}
fn pack(c: vec4<u32>) -> u32 {
    return c.x | (c.y << 8u) | (c.z << 16u) | (c.w << 24u);
}
fn absdiff(a: u32, b: u32) -> u32 {
    return select(b - a, a - b, a > b);
}
fn channel_delta(l: vec4<u32>, r: vec4<u32>) -> u32 {
    var d = max(max(absdiff(l.x, r.x), absdiff(l.y, r.y)), absdiff(l.z, r.z));
    if (params.compare_alpha != 0u) {
        d = max(d, absdiff(l.w, r.w));
    }
    return d;
}
fn washed_gray(p: vec4<u32>) -> vec4<u32> {
    let lum = (77u * p.x + 150u * p.y + 29u * p.z) >> 8u;
    let v = (lum >> 1u) + 64u;
    return vec4<u32>(v, v, v, 255u);
}
fn range_color(delta: u32) -> vec4<u32> {
    if (delta == 0u || delta < params.range_min) {
        return vec4<u32>(0u, 0u, 0u, 255u);
    }
    var t = 207u;  // collapsed window (min >= max): full brightness
    if (params.range_max > params.range_min) {
        let clipped = min(delta, params.range_max);
        t = min(((clipped - params.range_min) * 207u) / (params.range_max - params.range_min), 207u);
    }
    let v = 48u + t;
    return vec4<u32>(v, v, 0u, 255u);
}
fn mix_px(l: vec4<u32>, r: vec4<u32>) -> vec4<u32> {
    let w = params.weight256;
    return (l * (256u - w) + r * w + vec4<u32>(128u)) >> vec4<u32>(8u);
}

@compute @workgroup_size(16, 16, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>,
        @builtin(local_invocation_index) li: u32) {
    if (li < 6u) {
        atomicStore(&wg[li], 0u);
    }
    workgroupBarrier();

    if (gid.x < params.canvas_w && gid.y < params.canvas_h) {
        let idx = gid.y * params.canvas_w + gid.x;
        let l = unpack(left_px[idx]);
        let rx = i32(gid.x) - params.offset_x;
        let ry = i32(gid.y) - params.offset_y;
        var px = l;
        if (rx >= 0 && ry >= 0 && u32(rx) < params.right_w && u32(ry) < params.right_h) {
            let r = unpack(right_px[u32(ry) * params.right_w + u32(rx)]);
            let d = channel_delta(l, r);
            atomicAdd(&wg[0], 1u);
            if (d == 0u) {
                atomicAdd(&wg[1], 1u);
            } else if (d <= params.tolerance) {
                atomicAdd(&wg[2], 1u);
            } else {
                atomicAdd(&wg[3], 1u);
            }
            atomicAdd(&wg[4], d);
            atomicMax(&wg[5], d);

            if (params.mode == 0u) {
                if (d == 0u) {
                    px = washed_gray(l);
                } else if (d <= params.tolerance) {
                    px = vec4<u32>(0u, 0u, 255u, 255u);
                } else {
                    px = vec4<u32>(255u, 0u, 0u, 255u);
                }
            } else if (params.mode == 1u) {
                px = range_color(d);
            } else {
                px = mix_px(l, r);
            }
        } else if (params.mode == 1u) {
            px = vec4<u32>(0u, 0u, 0u, 255u);
        }
        out_px[idx] = pack(px);
    }

    workgroupBarrier();
    if (li == 0u) {
        atomicAdd(&stats[0], atomicLoad(&wg[0]));
        atomicAdd(&stats[1], atomicLoad(&wg[1]));
        atomicAdd(&stats[2], atomicLoad(&wg[2]));
        atomicAdd(&stats[3], atomicLoad(&wg[3]));
        let add = atomicLoad(&wg[4]);
        let old = atomicAdd(&stats[4], add);
        if (old + add < old) {
            atomicAdd(&stats[5], 1u);
        }
        atomicMax(&stats[6], atomicLoad(&wg[5]));
    }
}

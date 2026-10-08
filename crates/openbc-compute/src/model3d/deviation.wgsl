struct Node { bmin: vec3<f32>, a: u32, bmax: vec3<f32>, count: u32 };
struct Params { count: u32, p0: u32, p1: u32, p2: u32 };
@group(0) @binding(0) var<storage, read> nodes: array<Node>;
@group(0) @binding(1) var<storage, read> tris: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> queries: array<vec4<f32>>;
@group(0) @binding(3) var<storage, read_write> outd: array<f32>;
@group(0) @binding(4) var<uniform> params: Params;

fn box_d2(p: vec3<f32>, n: Node) -> f32 {
    let d = max(max(n.bmin - p, p - n.bmax), vec3<f32>(0.0));
    return dot(d, d);
}

fn closest_tri(p: vec3<f32>, a: vec3<f32>, b: vec3<f32>, c: vec3<f32>) -> vec3<f32> {
    let ab = b - a; let ac = c - a; let ap = p - a;
    let d1 = dot(ab, ap); let d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) { return a; }
    let bp = p - b;
    let d3 = dot(ab, bp); let d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) { return b; }
    let vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) { return a + ab * (d1 / (d1 - d3)); }
    let cp = p - c;
    let d5 = dot(ab, cp); let d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) { return c; }
    let vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) { return a + ac * (d2 / (d2 - d6)); }
    let va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    let denom = 1.0 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= params.count) { return; }
    let p = queries[i].xyz;
    var best = 3.4e38;
    var best_n = vec3<f32>(0.0);
    var best_q = p;
    var stack: array<u32, 64>;
    var sp = 1u;
    stack[0] = 0u;
    loop {
        if (sp == 0u) { break; }
        sp = sp - 1u;
        let ni = stack[sp];
        let nd = nodes[ni];
        if (box_d2(p, nd) >= best) { continue; }
        if (nd.count > 0u) {
            for (var k = nd.a; k < nd.a + nd.count; k = k + 1u) {
                let a = tris[3u * k].xyz; let b = tris[3u * k + 1u].xyz; let c = tris[3u * k + 2u].xyz;
                let q = closest_tri(p, a, b, c);
                let d = q - p;
                let d2 = dot(d, d);
                if (d2 < best) { best = d2; best_q = q; best_n = cross(b - a, c - a); }
            }
        } else {
            let l = ni + 1u; let r = nd.a;
            let dl = box_d2(p, nodes[l]); let dr = box_d2(p, nodes[r]);
            if (dl < dr) {
                if (dr < best) { stack[sp] = r; sp = sp + 1u; }
                if (dl < best) { stack[sp] = l; sp = sp + 1u; }
            } else {
                if (dl < best) { stack[sp] = l; sp = sp + 1u; }
                if (dr < best) { stack[sp] = r; sp = sp + 1u; }
            }
        }
    }
    let s = select(1.0, -1.0, dot(p - best_q, best_n) < 0.0);
    outd[i] = s * sqrt(best);
}


@group(0) @binding(0) var<storage, read> input_bytes: array<u32>;
@group(0) @binding(1) var<storage, read_write> output_hashes: array<atomic<u32>>;

@compute @workgroup_size(256)
fn hash_blocks(@builtin(global_invocation_id) id: vec3<u32>) {
    let index = id.x;
    if (index < arrayLength(&input_bytes)) {
        atomicAdd(&output_hashes[index], input_bytes[index]);
    }
}

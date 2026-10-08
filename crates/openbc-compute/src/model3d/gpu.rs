//! wgpu compute backend for signed-distance queries (cargo feature `gpu`).
//! Written against wgpu 24 (`Maintain::Wait`, `Option` entry points); newer wgpu renames a few
//! of these calls - adjust `try_new` / `signed_distances` accordingly.
//!
//! * the BVH nodes and triangles are uploaded once per call as storage buffers,
//! * queries stream through in bounded chunks (`CHUNK` points per dispatch),
//! * anything that does not fit the device limits returns `GpuError::TooLarge` and the caller
//!   falls back to the CPU.
//! GPU results are f32 and traversal order differs, so they match the CPU result to ~1e-5 mm
//! relative, not bit-for-bit (unlike the integer-only image path).
use super::backend::{DeviationExecutor, GpuError};
use super::bvh::Bvh;
use openbc_core::model3d::{CancelToken, V3};
use std::sync::Arc;

const CHUNK: usize = 1 << 20;
const WORKGROUP: u32 = 64;

/// Signed-distance compute shader (`deviation.wgsl`).
pub const WGSL: &str = include_str!("deviation.wgsl");

pub struct WgpuDeviation {
    device: Arc<wgpu::Device>,
    queue: Arc<wgpu::Queue>,
    pipeline: wgpu::ComputePipeline,
    layout: wgpu::BindGroupLayout,
    name: String,
}

fn dev(e: impl std::fmt::Display) -> GpuError { GpuError::Device(e.to_string()) }

impl WgpuDeviation {
    /// Requests its own adapter/device. Software adapters are rejected (the CPU path is faster).
    pub fn try_new() -> Result<Self, GpuError> {
        let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor::default());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            force_fallback_adapter: false,
            compatible_surface: None,
        })).ok_or_else(|| GpuError::Unavailable("no compatible adapter".into()))?;
        let info = adapter.get_info();
        if info.device_type == wgpu::DeviceType::Cpu { return Err(GpuError::Unavailable("software adapter".into())); }
        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: Some("openbc-model3d"),
            required_features: wgpu::Features::empty(),
            required_limits: adapter.limits(),
            memory_hints: wgpu::MemoryHints::Performance,
        }, None)).map_err(dev)?;
        Self::from_device(Arc::new(device), Arc::new(queue), info.name)
    }

    /// Reuse the application's existing device (the HAL in openbc-compute already owns one).
    pub fn from_device(device: Arc<wgpu::Device>, queue: Arc<wgpu::Queue>, name: String) -> Result<Self, GpuError> {
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("model3d-deviation"),
            source: wgpu::ShaderSource::Wgsl(WGSL.into()),
        });
        let entry = |binding, ty| wgpu::BindGroupLayoutEntry {
            binding, visibility: wgpu::ShaderStages::COMPUTE, ty, count: None,
        };
        let storage = |ro| wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Storage { read_only: ro }, has_dynamic_offset: false, min_binding_size: None };
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: None,
            entries: &[
                entry(0, storage(true)), entry(1, storage(true)), entry(2, storage(true)), entry(3, storage(false)),
                entry(4, wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }),
            ],
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: None, bind_group_layouts: &[&layout], push_constant_ranges: &[] });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("model3d-deviation"), layout: Some(&pl), module: &module, entry_point: Some("main"),
            compilation_options: Default::default(), cache: None,
        });
        Ok(Self { device, queue, pipeline, layout, name })
    }

    fn empty_buffer(&self, size: u64, usage: wgpu::BufferUsages) -> wgpu::Buffer {
        self.device.create_buffer(&wgpu::BufferDescriptor { label: None, size, usage, mapped_at_creation: false })
    }

    fn buffer(&self, data: &[u8], usage: wgpu::BufferUsages) -> wgpu::Buffer {
        use wgpu::util::DeviceExt;
        self.device.create_buffer_init(&wgpu::util::BufferInitDescriptor { label: None, contents: data, usage })
    }
}

impl DeviationExecutor for WgpuDeviation {
    fn name(&self) -> String { self.name.clone() }

    fn signed_distances(&self, bvh: &Bvh, queries: &[V3], cancel: &CancelToken) -> Result<Vec<f32>, GpuError> {
        if bvh.is_empty() { return Err(GpuError::Unavailable("empty BVH".into())); }
        let limits = self.device.limits();
        let node_words: Vec<[u32; 8]> = bvh.nodes.iter().map(|n| [
            n.min[0].to_bits(), n.min[1].to_bits(), n.min[2].to_bits(), n.a,
            n.max[0].to_bits(), n.max[1].to_bits(), n.max[2].to_bits(), n.count,
        ]).collect();
        let tris = bvh.gpu_triangles();
        let (nb, tb) = ((node_words.len() * 32) as u64, (tris.len() * 16) as u64);
        let cap = (limits.max_storage_buffer_binding_size as u64).min(limits.max_buffer_size);
        if nb > cap || tb > cap { return Err(GpuError::TooLarge("BVH exceeds device buffer limits".into())); }

        let nodes_buf = self.buffer(bytemuck::cast_slice(&node_words), wgpu::BufferUsages::STORAGE);
        let tris_buf = self.buffer(bytemuck::cast_slice(&tris), wgpu::BufferUsages::STORAGE);

        let mut out = Vec::with_capacity(queries.len());
        for chunk in queries.chunks(CHUNK) {
            if cancel.is_cancelled() { return Err(GpuError::Cancelled); }
            let q: Vec<[f32; 4]> = chunk.iter().map(|p| [p[0], p[1], p[2], 0.0]).collect();
            let bytes = (chunk.len() * 4) as u64;
            let q_buf = self.buffer(bytemuck::cast_slice(&q), wgpu::BufferUsages::STORAGE);
            let out_buf = self.empty_buffer(bytes, wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC);
            let stage = self.empty_buffer(bytes, wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST);
            let params = self.buffer(bytemuck::cast_slice(&[chunk.len() as u32, 0, 0, 0]), wgpu::BufferUsages::UNIFORM);
            let bind = self.device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: None, layout: &self.layout,
                entries: &[
                    wgpu::BindGroupEntry { binding: 0, resource: nodes_buf.as_entire_binding() },
                    wgpu::BindGroupEntry { binding: 1, resource: tris_buf.as_entire_binding() },
                    wgpu::BindGroupEntry { binding: 2, resource: q_buf.as_entire_binding() },
                    wgpu::BindGroupEntry { binding: 3, resource: out_buf.as_entire_binding() },
                    wgpu::BindGroupEntry { binding: 4, resource: params.as_entire_binding() },
                ],
            });
            let mut enc = self.device.create_command_encoder(&Default::default());
            {
                let mut pass = enc.begin_compute_pass(&Default::default());
                pass.set_pipeline(&self.pipeline);
                pass.set_bind_group(0, &bind, &[]);
                pass.dispatch_workgroups((chunk.len() as u32 + WORKGROUP - 1) / WORKGROUP, 1, 1);
            }
            enc.copy_buffer_to_buffer(&out_buf, 0, &stage, 0, bytes);
            self.queue.submit(Some(enc.finish()));
            let (tx, rx) = std::sync::mpsc::channel();
            stage.slice(..).map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
            self.device.poll(wgpu::Maintain::Wait);
            rx.recv().map_err(dev)?.map_err(dev)?;
            {
                let view = stage.slice(..).get_mapped_range();
                out.extend_from_slice(bytemuck::cast_slice::<u8, f32>(&view));
            }
            stage.unmap();
        }
        Ok(out)
    }
}

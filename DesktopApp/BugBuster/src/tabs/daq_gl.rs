// =============================================================================
// daq_gl.rs — WebGL2 trace renderer for the high-speed DAQ tab.
//
// Draws the per-lane min/max envelopes of the analog tracks as GL_LINES (one
// vertical segment per pixel column = a filled envelope). Per-vertex colour
// lets the current track be tinted by fusion source (FINE / COARSE / BLEND).
// Axes, labels, cursor, selection and the dI/dt heatmap are drawn on a separate
// 2D overlay canvas by daq.rs.
// =============================================================================

use wasm_bindgen::JsCast;
use web_sys::{
    HtmlCanvasElement, WebGl2RenderingContext as Gl, WebGlBuffer, WebGlProgram, WebGlShader,
};

/// One stacked lane to render.
pub struct Lane<'a> {
    /// Pixel top / bottom of the lane's plot region (y grows downward).
    pub y_top: f32,
    pub y_bottom: f32,
    /// Per-column envelope (already clipped to the visible window).
    pub vmin: &'a [f32],
    pub vmax: &'a [f32],
    /// Optional per-column fusion source for tinting (same length as vmin).
    pub source: Option<&'a [u8]>,
    /// Value range mapped to [y_bottom, y_top].
    pub lo: f32,
    pub hi: f32,
    /// Base RGB colour (0..1).
    pub color: [f32; 3],
    /// Tint by fusion source when `source` is set.
    pub tint: bool,
}

pub struct GlRenderer {
    ctx: Gl,
    program: WebGlProgram,
    buffer: WebGlBuffer,
    a_pos: u32,
    a_color: u32,
}

const VERT_SRC: &str = r#"#version 300 es
in vec2 a_pos;
in vec3 a_color;
out vec3 v_color;
void main() {
    v_color = a_color;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
"#;

const FRAG_SRC: &str = r#"#version 300 es
precision mediump float;
in vec3 v_color;
out vec4 o_color;
void main() {
    o_color = vec4(v_color, 0.92);
}
"#;

impl GlRenderer {
    pub fn new(canvas: &HtmlCanvasElement) -> Option<Self> {
        let ctx = canvas
            .get_context("webgl2")
            .ok()
            .flatten()?
            .dyn_into::<Gl>()
            .ok()?;
        let vert = compile(&ctx, Gl::VERTEX_SHADER, VERT_SRC)?;
        let frag = compile(&ctx, Gl::FRAGMENT_SHADER, FRAG_SRC)?;
        let program = ctx.create_program()?;
        ctx.attach_shader(&program, &vert);
        ctx.attach_shader(&program, &frag);
        ctx.link_program(&program);
        if !ctx
            .get_program_parameter(&program, Gl::LINK_STATUS)
            .as_bool()
            .unwrap_or(false)
        {
            web_sys::console::warn_1(
                &ctx.get_program_info_log(&program)
                    .unwrap_or_default()
                    .into(),
            );
            return None;
        }
        let a_pos = ctx.get_attrib_location(&program, "a_pos") as u32;
        let a_color = ctx.get_attrib_location(&program, "a_color") as u32;
        let buffer = ctx.create_buffer()?;
        ctx.enable(Gl::BLEND);
        ctx.blend_func(Gl::SRC_ALPHA, Gl::ONE_MINUS_SRC_ALPHA);
        Some(Self {
            ctx,
            program,
            buffer,
            a_pos,
            a_color,
        })
    }

    /// Render all lanes. `width`/`height` are the canvas pixel dimensions;
    /// `x0`/`x1` are the left/right pixel bounds of the plot area (the area to
    /// the left of `x0` is reserved for lane labels and drawn by the overlay).
    pub fn render(&self, width: f32, height: f32, x0: f32, x1: f32, lanes: &[Lane]) {
        let ctx = &self.ctx;
        ctx.viewport(0, 0, width as i32, height as i32);
        ctx.clear_color(0.04, 0.06, 0.10, 1.0);
        ctx.clear(Gl::COLOR_BUFFER_BIT);
        if width <= 0.0 || height <= 0.0 {
            return;
        }
        let plot_w = (x1 - x0).max(1.0);

        // Interleaved [x, y, r, g, b] per vertex. Each column contributes a
        // vertical min/max envelope segment AND a connector to the previous
        // column's midpoint, so the trace stays continuous even when fully
        // zoomed in (where min ≈ max and the envelope alone would vanish).
        let mut verts: Vec<f32> = Vec::new();
        for lane in lanes {
            let n = lane.vmin.len().min(lane.vmax.len());
            if n == 0 {
                continue;
            }
            let span = (lane.hi - lane.lo).abs().max(1e-12);
            let denom = if n > 1 { (n - 1) as f32 } else { 1.0 };
            let map_y = |v: f32| -> f32 {
                let t = ((v - lane.lo) / span).clamp(0.0, 1.0);
                let py = lane.y_bottom - t * (lane.y_bottom - lane.y_top);
                1.0 - (py / height) * 2.0
            };
            let mut prev: Option<(f32, f32)> = None; // (x_ndc, mid_ndc)
            for i in 0..n {
                let px = x0 + (i as f32 / denom) * plot_w;
                let x_ndc = (px / width) * 2.0 - 1.0;
                let y0 = map_y(lane.vmin[i]);
                let y1 = map_y(lane.vmax[i]);
                let mid = (y0 + y1) * 0.5;

                let color = if lane.tint {
                    match lane.source.and_then(|s| s.get(i)).copied().unwrap_or(0) {
                        1 => [0.96, 0.62, 0.12], // COARSE — amber
                        2 => [0.66, 0.33, 0.97], // BLEND — purple
                        _ => lane.color,         // FINE — base
                    }
                } else {
                    lane.color
                };
                let c = color;
                // Vertical envelope segment for this column.
                verts.extend_from_slice(&[x_ndc, y0, c[0], c[1], c[2]]);
                verts.extend_from_slice(&[x_ndc, y1, c[0], c[1], c[2]]);
                // Connector from the previous column's midpoint to this one.
                if let Some((px_prev, mid_prev)) = prev {
                    verts.extend_from_slice(&[px_prev, mid_prev, c[0], c[1], c[2]]);
                    verts.extend_from_slice(&[x_ndc, mid, c[0], c[1], c[2]]);
                }
                prev = Some((x_ndc, mid));
            }
        }
        if verts.is_empty() {
            return;
        }

        ctx.use_program(Some(&self.program));
        ctx.bind_buffer(Gl::ARRAY_BUFFER, Some(&self.buffer));
        unsafe {
            let arr = js_sys::Float32Array::view(&verts);
            ctx.buffer_data_with_array_buffer_view(Gl::ARRAY_BUFFER, &arr, Gl::DYNAMIC_DRAW);
        }
        let stride = 5 * 4; // 5 floats
        ctx.enable_vertex_attrib_array(self.a_pos);
        ctx.vertex_attrib_pointer_with_i32(self.a_pos, 2, Gl::FLOAT, false, stride, 0);
        ctx.enable_vertex_attrib_array(self.a_color);
        ctx.vertex_attrib_pointer_with_i32(self.a_color, 3, Gl::FLOAT, false, stride, 2 * 4);
        let count = (verts.len() / 5) as i32;
        ctx.draw_arrays(Gl::LINES, 0, count);
    }
}

fn compile(ctx: &Gl, kind: u32, src: &str) -> Option<WebGlShader> {
    let sh = ctx.create_shader(kind)?;
    ctx.shader_source(&sh, src);
    ctx.compile_shader(&sh);
    if !ctx
        .get_shader_parameter(&sh, Gl::COMPILE_STATUS)
        .as_bool()
        .unwrap_or(false)
    {
        web_sys::console::warn_1(&ctx.get_shader_info_log(&sh).unwrap_or_default().into());
        return None;
    }
    Some(sh)
}

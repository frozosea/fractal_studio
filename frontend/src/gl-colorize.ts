// gl-colorize.ts
//
// GPU-accelerated fractal colorization via WebGL2 fragment shaders.
// Ports all 11 colormaps from backend/src/compute/colormap.hpp to GLSL ES 3.00,
// producing pixel-identical output (modulo GPU float rounding) to the C++ path.

const VERT_SRC = `#version 300 es
precision highp float;

layout(location = 0) in vec2 a_pos;

void main() {
  gl_Position = vec4(a_pos, 0.0, 1.0);
}
`;

const FRAG_SRC = `#version 300 es
precision highp float;
precision highp int;

uniform highp usampler2D u_iter;
uniform highp sampler2D  u_norm;
uniform int  u_maxIter;
uniform int  u_colormap;
uniform bool u_smooth;

out vec4 fragColor;

const float PI = 3.141592653589793;

// ── helpers ──────────────────────────────────────────────────────────────────

int clamp255(int v) {
  return clamp(v, 0, 255);
}

float cos_color(float n, float freq) {
  return 128.0 - 128.0 * cos(freq * n * PI);
}

float smooth_mu(int iter, float norm) {
  if (norm > 1.0) {
    float mu = float(iter) + 1.0 - log2(log2(norm));
    return max(mu, 0.0);
  }
  return float(iter);
}

// ── HSV → RGB ────────────────────────────────────────────────────────────────

vec3 hsv_to_rgb(float h, float s, float v) {
  float c  = v * s;
  float hh = h / 60.0;
  float x  = c * (1.0 - abs(mod(hh, 2.0) - 1.0));

  float rr = 0.0, gg = 0.0, bb = 0.0;
  if      (hh < 1.0) { rr = c; gg = x; }
  else if (hh < 2.0) { rr = x; gg = c; }
  else if (hh < 3.0) { gg = c; bb = x; }
  else if (hh < 4.0) { gg = x; bb = c; }
  else if (hh < 5.0) { rr = x; bb = c; }
  else               { rr = c; bb = x; }

  float m = v - c;
  return vec3(
    float(clamp255(int((rr + m) * 255.0))),
    float(clamp255(int((gg + m) * 255.0))),
    float(clamp255(int((bb + m) * 255.0)))
  );
}

// ── hue1530: 6-segment fully-saturated cyclic hue wheel (green at 0) ─────

vec3 hue1530(int idx) {
  idx = idx - (idx / 1530) * 1530;       // idx % 1530
  if (idx < 0) idx += 1530;
  int seg = idx / 255;
  int d   = idx - seg * 255;             // idx % 255
  int rr = 0, gg = 0, bb = 0;
  if      (seg == 0) { rr = 0;       gg = 255;     bb = d;       }  // G→C
  else if (seg == 1) { rr = 0;       gg = 255 - d; bb = 255;     }  // C→B
  else if (seg == 2) { rr = d;       gg = 0;       bb = 255;     }  // B→P
  else if (seg == 3) { rr = 255;     gg = 0;       bb = 255 - d; }  // P→R
  else if (seg == 4) { rr = 255;     gg = d;       bb = 0;       }  // R→Y
  else               { rr = 255 - d; gg = 255;     bb = 0;       }  // Y→G
  return vec3(float(rr), float(gg), float(bb));
}

// ── Gradient-stop interpolation (returns RGB 0-255) ──────────────────────

// Each palette is encoded as parallel float arrays for t, r, g, b.
// Linear interpolation between surrounding stops, clamped at boundaries.

vec3 gradient_interp(float t, float ts[9], float rs[9], float gs[9], float bs[9], int n) {
  t = clamp(t, 0.0, 1.0);
  if (t <= ts[0]) return vec3(rs[0], gs[0], bs[0]);
  for (int i = 1; i < 9; i++) {
    if (i >= n) break;
    if (t <= ts[i]) {
      float span = max(1e-12, ts[i] - ts[i - 1]);
      float u = (t - ts[i - 1]) / span;
      return vec3(
        float(clamp255(int(round(rs[i - 1] * (1.0 - u) + rs[i] * u)))),
        float(clamp255(int(round(gs[i - 1] * (1.0 - u) + gs[i] * u)))),
        float(clamp255(int(round(bs[i - 1] * (1.0 - u) + bs[i] * u))))
      );
    }
  }
  return vec3(rs[n - 1], gs[n - 1], bs[n - 1]);
}

// ── gradient palette dispatch (returns rgb 0-255, w=-1 if not a gradient) ─

vec4 gradient_palette(float t, int palette) {
  // Pad all arrays to length 9 (max stops).
  float ts[9], rs[9], gs[9], bs[9];

  if (palette == 6) {          // Inferno (9 stops)
    ts[0]=0.00; rs[0]=  0.0; gs[0]=  0.0; bs[0]=  4.0;
    ts[1]=0.14; rs[1]= 31.0; gs[1]= 12.0; bs[1]= 72.0;
    ts[2]=0.28; rs[2]= 85.0; gs[2]= 15.0; bs[2]=109.0;
    ts[3]=0.42; rs[3]=136.0; gs[3]= 34.0; bs[3]=106.0;
    ts[4]=0.56; rs[4]=186.0; gs[4]= 54.0; bs[4]= 85.0;
    ts[5]=0.70; rs[5]=227.0; gs[5]= 89.0; bs[5]= 51.0;
    ts[6]=0.84; rs[6]=249.0; gs[6]=140.0; bs[6]= 10.0;
    ts[7]=0.94; rs[7]=252.0; gs[7]=195.0; bs[7]= 55.0;
    ts[8]=1.00; rs[8]=252.0; gs[8]=255.0; bs[8]=164.0;
    return vec4(gradient_interp(t, ts, rs, gs, bs, 9), 1.0);
  }
  if (palette == 7) {          // Viridis (5 stops)
    ts[0]=0.00; rs[0]= 68.0; gs[0]=  1.0; bs[0]= 84.0;
    ts[1]=0.25; rs[1]= 59.0; gs[1]= 82.0; bs[1]=139.0;
    ts[2]=0.50; rs[2]= 33.0; gs[2]=145.0; bs[2]=140.0;
    ts[3]=0.75; rs[3]= 94.0; gs[3]=201.0; bs[3]= 98.0;
    ts[4]=1.00; rs[4]=253.0; gs[4]=231.0; bs[4]= 37.0;
    // fill unused
    ts[5]=1.0; rs[5]=0.0; gs[5]=0.0; bs[5]=0.0;
    ts[6]=1.0; rs[6]=0.0; gs[6]=0.0; bs[6]=0.0;
    ts[7]=1.0; rs[7]=0.0; gs[7]=0.0; bs[7]=0.0;
    ts[8]=1.0; rs[8]=0.0; gs[8]=0.0; bs[8]=0.0;
    return vec4(gradient_interp(t, ts, rs, gs, bs, 5), 1.0);
  }
  if (palette == 8) {          // Twilight (7 stops)
    ts[0]=0.00; rs[0]= 32.0; gs[0]= 24.0; bs[0]= 70.0;
    ts[1]=0.18; rs[1]= 63.0; gs[1]= 92.0; bs[1]=180.0;
    ts[2]=0.36; rs[2]= 58.0; gs[2]=150.0; bs[2]=165.0;
    ts[3]=0.54; rs[3]=240.0; gs[3]=210.0; bs[3]=120.0;
    ts[4]=0.72; rs[4]=210.0; gs[4]= 90.0; bs[4]= 90.0;
    ts[5]=0.88; rs[5]= 90.0; gs[5]= 50.0; bs[5]=110.0;
    ts[6]=1.00; rs[6]= 32.0; gs[6]= 24.0; bs[6]= 70.0;
    ts[7]=1.0; rs[7]=0.0; gs[7]=0.0; bs[7]=0.0;
    ts[8]=1.0; rs[8]=0.0; gs[8]=0.0; bs[8]=0.0;
    return vec4(gradient_interp(t, ts, rs, gs, bs, 7), 1.0);
  }
  if (palette == 9) {          // EmberBlue (5 stops)
    ts[0]=0.00; rs[0]=  5.0; gs[0]=  8.0; bs[0]= 32.0;
    ts[1]=0.22; rs[1]= 10.0; gs[1]= 70.0; bs[1]=120.0;
    ts[2]=0.48; rs[2]= 55.0; gs[2]=190.0; bs[2]=185.0;
    ts[3]=0.72; rs[3]=245.0; gs[3]=172.0; bs[3]= 75.0;
    ts[4]=1.00; rs[4]=255.0; gs[4]=246.0; bs[4]=210.0;
    ts[5]=1.0; rs[5]=0.0; gs[5]=0.0; bs[5]=0.0;
    ts[6]=1.0; rs[6]=0.0; gs[6]=0.0; bs[6]=0.0;
    ts[7]=1.0; rs[7]=0.0; gs[7]=0.0; bs[7]=0.0;
    ts[8]=1.0; rs[8]=0.0; gs[8]=0.0; bs[8]=0.0;
    return vec4(gradient_interp(t, ts, rs, gs, bs, 5), 1.0);
  }
  return vec4(-1.0);  // not a gradient palette
}

// ── main ─────────────────────────────────────────────────────────────────────

void main() {
  // Backend fields and Canvas ImageData are top-to-bottom (row 0 is the
  // viewport's +imaginary/top edge), while WebGL fragment coordinates start
  // at the bottom. Sample the opposite texture row so WebGL colorization has
  // exactly the same orientation as render-inline/progressive RGBA frames.
  ivec2 fieldSize = textureSize(u_iter, 0);
  ivec2 coord = ivec2(
    int(gl_FragCoord.x),
    fieldSize.y - 1 - int(gl_FragCoord.y)
  );
  uint  iterU = texelFetch(u_iter, coord, 0).r;
  float norm  = texelFetch(u_norm, coord, 0).r;
  int   iter  = int(iterU);

  // Interior: white
  if (iter >= u_maxIter) {
    fragColor = vec4(1.0, 1.0, 1.0, 1.0);
    return;
  }

  float r, g, b;

  if (u_smooth) {
    float mu = smooth_mu(iter, norm);
    float t  = fract(mu / 32.0);

    // Try gradient palettes first
    vec4 gp = gradient_palette(t, u_colormap);
    if (gp.w > 0.0) {
      fragColor = vec4(gp.rgb / 255.0, 1.0);
      return;
    }

    if (u_colormap == 2) {          // HsvWheel
      vec3 c = hsv_to_rgb(t * 360.0, 1.0, 1.0);
      fragColor = vec4(c / 255.0, 1.0);
      return;
    }
    if (u_colormap == 3) {          // Tri765
      float mf  = t * 765.0;
      int   m   = int(mf);
      int   d   = int((mf - float(m)) * 255.0);
      int   band = (m / 255) % 3;
      int rr, gg, bb;
      if      (band == 0) { rr = 255 - d; gg = d;       bb = 255;     }
      else if (band == 1) { rr = d;       gg = 255;     bb = 255 - d; }
      else                { rr = 255;     gg = 255 - d; bb = d;       }
      fragColor = vec4(float(clamp255(rr)), float(clamp255(gg)), float(clamp255(bb)), 255.0) / 255.0;
      return;
    }
    if (u_colormap == 4) {          // Grayscale
      int v = clamp255(int(t * 255.0));
      fragColor = vec4(vec3(float(v) / 255.0), 1.0);
      return;
    }
    if (u_colormap == 10) {         // Spectral1530
      if (mu < 255.0) {
        fragColor = vec4(0.0, float(clamp255(int(mu))) / 255.0, 0.0, 1.0);
      } else {
        vec3 c = hue1530(int(mod(mu - 255.0, 1530.0)));
        fragColor = vec4(c / 255.0, 1.0);
      }
      return;
    }
    if (u_colormap == 1) {          // Mod17
      int idx = int(mu) - (int(mu) / 17) * 17;   // int(mu) % 17
      int v   = idx * 15;
      fragColor = vec4(vec3(float(clamp255(v)) / 255.0), 1.0);
      return;
    }
    // ClassicCos (0) and HsRainbow (5) fallback
    r = cos_color(t,  53.0);
    g = cos_color(t,  27.0);
    b = cos_color(t, 139.0);
    fragColor = vec4(
      float(clamp255(int(r))) / 255.0,
      float(clamp255(int(g))) / 255.0,
      float(clamp255(int(b))) / 255.0,
      1.0
    );
    return;
  }

  // ── Non-smooth path ────────────────────────────────────────────────────

  float n = (float(iter) + 1.0) / (float(u_maxIter) + 2.0);

  // Try gradient palettes first
  vec4 gp = gradient_palette(n, u_colormap);
  if (gp.w > 0.0) {
    fragColor = vec4(gp.rgb / 255.0, 1.0);
    return;
  }

  if (u_colormap == 1) {            // Mod17
    fragColor = vec4(
      float(clamp255(iter - (iter / 256) * 256)) / 255.0,    // iter % 256
      float(clamp255(iter / 256)) / 255.0,
      float(clamp255((iter - (iter / 17) * 17) * 17)) / 255.0, // (iter%17)*17
      1.0
    );
    return;
  }
  if (u_colormap == 2) {            // HsvWheel
    float h = float(iter - (iter / 1440) * 1440) / 4.0;   // fmod(iter, 1440)/4
    vec3 c = hsv_to_rgb(h, 1.0, 1.0);
    fragColor = vec4(c / 255.0, 1.0);
    return;
  }
  if (u_colormap == 3) {            // Tri765
    int m    = iter - (iter / 765) * 765;                  // iter % 765
    int band = m / 255;
    int d    = m - band * 255;                             // m % 255
    int rr, gg, bb;
    if      (band == 0) { rr = 255 - d; gg = d;       bb = 255;     }
    else if (band == 1) { rr = d;       gg = 255;     bb = 255 - d; }
    else                { rr = 255;     gg = 255 - d; bb = d;       }
    fragColor = vec4(float(clamp255(rr)), float(clamp255(gg)), float(clamp255(bb)), 255.0) / 255.0;
    return;
  }
  if (u_colormap == 4) {            // Grayscale
    int v = clamp255(int(n * 255.0));
    fragColor = vec4(vec3(float(v) / 255.0), 1.0);
    return;
  }
  if (u_colormap == 10) {           // Spectral1530
    if (iter < 255) {
      fragColor = vec4(0.0, float(iter) / 255.0, 0.0, 1.0);
    } else {
      int idx = (iter - 255) - ((iter - 255) / 1530) * 1530;  // (iter-255)%1530
      vec3 c = hue1530(idx);
      fragColor = vec4(c / 255.0, 1.0);
    }
    return;
  }
  // ClassicCos (0) and HsRainbow (5) fallback
  r = cos_color(n,  53.0);
  g = cos_color(n,  27.0);
  b = cos_color(n, 139.0);
  fragColor = vec4(
    float(clamp255(int(r))) / 255.0,
    float(clamp255(int(g))) / 255.0,
    float(clamp255(int(b))) / 255.0,
    1.0
  );
}
`;

// ── Colormap name → integer ID ───────────────────────────────────────────────

const COLORMAP_IDS: Record<string, number> = {
  classic_cos:  0,
  mod17:        1,
  hsv_wheel:    2,
  tri765:       3,
  grayscale:    4,
  hs_rainbow:   5,
  inferno:      6,
  viridis:      7,
  twilight:     8,
  ember_blue:   9,
  spectral1530: 10,
};

// ── WebGL2 availability check ────────────────────────────────────────────────

export function isWebGL2Available(): boolean {
  try {
    const c = document.createElement('canvas');
    const gl = c.getContext('webgl2');
    if (gl) {
      const ext = gl.getExtension('WEBGL_lose_context');
      if (ext) ext.loseContext();
      return true;
    }
  } catch { /* ignore */ }
  return false;
}

// ── Shader helpers ───────────────────────────────────────────────────────────

function compileShader(gl: WebGL2RenderingContext, type: number, src: string): WebGLShader {
  const shader = gl.createShader(type);
  if (!shader) throw new Error('Failed to create shader');
  gl.shaderSource(shader, src);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(shader) ?? '';
    gl.deleteShader(shader);
    console.error(`Shader compile error:\n${log}`);
    throw new Error(`Shader compile failed: ${log}`);
  }
  return shader;
}

function linkProgram(gl: WebGL2RenderingContext, vs: WebGLShader, fs: WebGLShader): WebGLProgram {
  const prog = gl.createProgram();
  if (!prog) throw new Error('Failed to create program');
  gl.attachShader(prog, vs);
  gl.attachShader(prog, fs);
  gl.linkProgram(prog);
  if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
    const log = gl.getProgramInfoLog(prog) ?? '';
    gl.deleteProgram(prog);
    console.error(`Program link error:\n${log}`);
    throw new Error(`Program link failed: ${log}`);
  }
  return prog;
}

// ── GlColorizer ──────────────────────────────────────────────────────────────

export class GlColorizer {
  private gl: WebGL2RenderingContext;
  private program: WebGLProgram;
  private vao: WebGLVertexArrayObject;
  private vbo: WebGLBuffer;
  private iterTex: WebGLTexture;
  private normTex: WebGLTexture;

  // Uniform locations
  private uIter: WebGLUniformLocation | null;
  private uNorm: WebGLUniformLocation | null;
  private uMaxIter: WebGLUniformLocation | null;
  private uColormap: WebGLUniformLocation | null;
  private uSmooth: WebGLUniformLocation | null;

  private fieldW = 0;
  private fieldH = 0;

  constructor(canvas: HTMLCanvasElement) {
    const gl = canvas.getContext('webgl2', {
      premultipliedAlpha: false,
      alpha: false,
    });
    if (!gl) throw new Error('WebGL2 not available');
    this.gl = gl;

    // Check for required texture format support (R32UI, R32F)
    // R32UI and R32F are core in WebGL2 — but verify renderable.
    // color_buffer_float is needed for R32F rendering targets (we only sample,
    // so this is informational, but texImage2D itself requires the format to be
    // accepted).

    // ── compile & link ─────────────────────────────────────────────────────
    const vs = compileShader(gl, gl.VERTEX_SHADER, VERT_SRC);
    const fs = compileShader(gl, gl.FRAGMENT_SHADER, FRAG_SRC);
    this.program = linkProgram(gl, vs, fs);
    gl.deleteShader(vs);
    gl.deleteShader(fs);

    // ── uniform locations ──────────────────────────────────────────────────
    this.uIter     = gl.getUniformLocation(this.program, 'u_iter');
    this.uNorm     = gl.getUniformLocation(this.program, 'u_norm');
    this.uMaxIter  = gl.getUniformLocation(this.program, 'u_maxIter');
    this.uColormap = gl.getUniformLocation(this.program, 'u_colormap');
    this.uSmooth   = gl.getUniformLocation(this.program, 'u_smooth');

    // ── fullscreen quad VAO ────────────────────────────────────────────────
    const vao = gl.createVertexArray();
    if (!vao) throw new Error('Failed to create VAO');
    this.vao = vao;

    const vbo = gl.createBuffer();
    if (!vbo) throw new Error('Failed to create VBO');
    this.vbo = vbo;

    // Two triangles covering [-1,1]x[-1,1]
    const quad = new Float32Array([
      -1, -1,   1, -1,   -1,  1,
       1, -1,   1,  1,   -1,  1,
    ]);

    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, quad, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.bindVertexArray(null);

    // ── textures ───────────────────────────────────────────────────────────
    const iterTex = gl.createTexture();
    if (!iterTex) throw new Error('Failed to create iter texture');
    this.iterTex = iterTex;

    const normTex = gl.createTexture();
    if (!normTex) throw new Error('Failed to create norm texture');
    this.normTex = normTex;

    // Initialize both textures with NEAREST filtering
    for (const tex of [this.iterTex, this.normTex]) {
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    }
    gl.bindTexture(gl.TEXTURE_2D, null);
  }

  /**
   * Upload iteration-count and norm (|z|^2) fields to GPU textures.
   * Both arrays are row-major, w*h elements.
   */
  uploadField(iter: Uint32Array, norm: Float32Array, w: number, h: number): void {
    const gl = this.gl;
    this.fieldW = w;
    this.fieldH = h;

    // iter → R32UI
    gl.bindTexture(gl.TEXTURE_2D, this.iterTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32UI, w, h, 0, gl.RED_INTEGER, gl.UNSIGNED_INT, iter);

    // norm → R32F
    gl.bindTexture(gl.TEXTURE_2D, this.normTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, w, h, 0, gl.RED, gl.FLOAT, norm);

    gl.bindTexture(gl.TEXTURE_2D, null);
  }

  /**
   * Render the colorized fractal to the canvas.
   */
  render(opts: { colormap: string; maxIter: number; smooth: boolean }): void {
    const gl = this.gl;

    // Resize viewport to match canvas drawing buffer
    gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);

    gl.useProgram(this.program);

    // Bind textures
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.iterTex);
    gl.uniform1i(this.uIter, 0);

    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.normTex);
    gl.uniform1i(this.uNorm, 1);

    // Set uniforms
    gl.uniform1i(this.uMaxIter, opts.maxIter);
    gl.uniform1i(this.uColormap, COLORMAP_IDS[opts.colormap] ?? 0);
    gl.uniform1i(this.uSmooth, opts.smooth ? 1 : 0);

    // Draw fullscreen quad
    gl.bindVertexArray(this.vao);
    gl.drawArrays(gl.TRIANGLES, 0, 6);
    gl.bindVertexArray(null);
  }

  /**
   * Release all GPU resources.
   */
  dispose(): void {
    const gl = this.gl;
    gl.deleteTexture(this.iterTex);
    gl.deleteTexture(this.normTex);
    gl.deleteProgram(this.program);
    gl.deleteBuffer(this.vbo);
    gl.deleteVertexArray(this.vao);
  }
}

// i18n.ts — two-language store (EN / 中文) with reactive locale.
// Lang is persisted in localStorage.

import { ref } from 'vue'

export type Lang = 'en' | 'zh'

function readSavedLang(): Lang {
  try { return (localStorage.getItem('fsd_lang') as Lang) || 'en' }
  catch { return 'en' }
}
export const lang = ref<Lang>(readSavedLang())

export function setLang(l: Lang) {
  lang.value = l
  localStorage.setItem('fsd_lang', l)
}

export function toggleLang() {
  setLang(lang.value === 'en' ? 'zh' : 'en')
}

type Dict = Record<string, { en: string; zh: string }>

const dict: Dict = {
  // ── Nav ─────────────────────────────────────────────────────────────────
  nav_map:          { en: 'Map',              zh: '图谱' },
  nav_points:       { en: 'Points',           zh: '特殊点' },
  nav_3d:           { en: '3D',               zh: '三维' },
  nav_runs:         { en: 'Runs',             zh: '运行记录' },
  nav_system:       { en: 'System',           zh: '系统' },
  nav_light:        { en: 'Light mode',       zh: '浅色模式' },
  nav_dark:         { en: 'Dark mode',        zh: '深色模式' },

  // ── Map controls ────────────────────────────────────────────────────────
  variant:          { en: 'Variant',          zh: '变体' },
  metric:           { en: 'Metric',           zh: '指标' },
  colormap:         { en: 'Colormap',         zh: '色图' },
  smooth:           { en: 'Smooth coloring', zh: '平滑着色' },
  iterations:       { en: 'Iterations',       zh: '迭代次数' },
  scale:            { en: 'Scale',            zh: '缩放' },
  center:           { en: 'Center',           zh: '中心' },
  theta:            { en: 'θ (rad)',          zh: 'θ（弧度）' },
  transition:       { en: '3D transition',     zh: '三维过渡' },
  julia:            { en: 'Julia set',        zh: 'Julia 集' },
  engine:           { en: 'Engine',           zh: '引擎' },
  scalar:           { en: 'Scalar',           zh: '标量类型' },
  reset:            { en: 'Reset',            zh: '重置视图' },
  reset_julia:      { en: 'Reset Julia view', zh: '重置 Julia 视图' },

  // ── Julia mode ──────────────────────────────────────────────────────────
  julia_selected_c: { en: 'Selected julia c', zh: '已选 Julia 常数 c' },
  julia_hint:       { en: 'Left-click picks c and recenters left map. Drag/Wheel works on both panes.',
                       zh: '左键单击选取 c 并重置左图中心，两侧均可拖拽/滚轮缩放。' },
  julia_left:       { en: 'Left',             zh: '左侧' },
  julia_right:      { en: 'Right: Julia',     zh: '右侧：Julia' },

  // ── Export ──────────────────────────────────────────────────────────────
  export_png:           { en: 'Export PNG',           zh: '导出 PNG' },
  export_video:         { en: 'Export Video',         zh: '导出视频' },
  export_julia_video:   { en: 'Export Julia Video',   zh: '导出 Julia 视频' },
  video_fps:            { en: 'FPS',                  zh: '帧率' },
  video_duration:       { en: 'Duration (s)',         zh: '时长（秒）' },
  video_seconds_per_octave: { en: 'Seconds per 2× zoom', zh: '每放大 2 倍（秒）' },
  video_estimate:       { en: 'Estimated',            zh: '预计' },
  video_start_frame:    { en: 'Start frame',          zh: '首帧' },
  video_end_frame:      { en: 'End frame',            zh: '尾帧' },
  video_preview:        { en: 'Preview',              zh: '预览' },
  video_width:          { en: 'Width px',             zh: '宽度（像素）' },
  video_height:         { en: 'Height px',            zh: '高度（像素）' },
  video_strip_width:    { en: 'Strip width px',       zh: '条带宽度（像素）' },
  video_depth:          { en: 'Depth (octaves)',      zh: '深度（倍频程）' },
  video_render:         { en: 'Render',               zh: '渲染' },
  video_cancel:         { en: 'Cancel',               zh: '取消' },
  video_download:       { en: 'Download video',       zh: '下载视频' },

  // ── 3D view ─────────────────────────────────────────────────────────────
  three_mode_hs:    { en: 'Hidden structure', zh: '隐结构' },
  three_mode_tx:    { en: 'Axis transition', zh: '轴向过渡' },
  three_metric:     { en: 'Metric',          zh: '指标' },
  three_resolution: { en: 'Resolution',      zh: '分辨率' },
  three_iso:        { en: 'Iso level',       zh: '等值面' },
  three_compute:    { en: 'Compute',         zh: '计算' },
  three_computing:  { en: 'Computing…',      zh: '计算中…' },
  three_center_re:  { en: 'Center Re',       zh: '中心实部' },
  three_center_im:  { en: 'Center Im',       zh: '中心虚部' },
  three_scale:      { en: 'Scale',           zh: '缩放' },

  // ── Runs / artifacts ────────────────────────────────────────────────────
  runs_title:       { en: 'Run History',      zh: '运行历史' },
  runs_id:          { en: 'Run ID',           zh: '运行 ID' },
  runs_module:      { en: 'Module',           zh: '模块' },
  runs_status:      { en: 'Status',           zh: '状态' },
  runs_artifacts:   { en: 'Artifacts',        zh: '产物' },
  runs_download:    { en: 'Download',         zh: '下载' },
  runs_none:        { en: 'No runs yet.',     zh: '暂无运行记录。' },

  // ── System ──────────────────────────────────────────────────────────────
  sys_title:        { en: 'System',           zh: '系统信息' },
  sys_cpu:          { en: 'CPU',              zh: 'CPU' },
  sys_gpu:          { en: 'GPU',              zh: 'GPU' },
  sys_ram:          { en: 'RAM',              zh: '内存' },
  sys_engines:      { en: 'Available engines', zh: '可用引擎' },

  // ── Status rail ─────────────────────────────────────────────────────────
  status_cpu:       { en: 'cpu',              zh: 'cpu' },
  status_gpu:       { en: 'gpu',              zh: 'gpu' },
  status_time:      { en: 'render',           zh: '渲染' },
  status_ready:     { en: 'ready',            zh: '就绪' },
  status_engine:    { en: 'engine',           zh: '引擎' },
  status_scalar:    { en: 'scalar',           zh: '标量' },

  // ── Metrics (display labels) ─────────────────────────────────────────────
  metric_escape:    { en: 'Escape time',      zh: '逃逸时间' },
  metric_min_abs:   { en: 'Min |z|',          zh: '最小 |z|' },
  metric_max_abs:   { en: 'Max |z|',          zh: '最大 |z|' },
  metric_envelope:  { en: 'Envelope',         zh: '包络' },
  metric_minpair:   { en: 'Min orbit distance', zh: '最小轨道距' },

  // ── Colormaps (display labels) ───────────────────────────────────────────
  cmap_classic_cos: { en: 'Classic Cos',      zh: '经典余弦' },
  cmap_mod17:       { en: 'Mod-17',           zh: 'Mod-17' },
  cmap_hsv_wheel:   { en: 'HSV Wheel',        zh: 'HSV 色轮' },
  cmap_tri765:      { en: 'Tri-765',          zh: 'Tri-765' },
  cmap_grayscale:   { en: 'Grayscale',        zh: '灰度' },
  cmap_hs_rainbow:  { en: 'HS Rainbow',       zh: '隐结构彩虹' },

  // ── Custom variants ──────────────────────────────────────────────────────
  custom_formula:   { en: 'Formula',          zh: '公式' },
  custom_name:      { en: 'Name',             zh: '名称' },
  custom_bailout:   { en: 'Escape radius',    zh: '逃逸半径' },
  custom_compile:   { en: 'Compile',          zh: '编译' },
  custom_delete:    { en: 'Delete',           zh: '删除' },
  custom_compiled:  { en: 'Compiled',         zh: '已编译' },
  custom_new:       { en: 'New custom…',      zh: '新建自定义…' },
  custom_hint:      { en: 'z and c are complex. Functions: sin cos tan exp log pow sqrt abs conj sinh cosh tanh. ^ = power.',
                       zh: 'z 和 c 为复数。函数：sin cos tan exp log pow sqrt abs conj sinh cosh tanh。^ = 幂运算。' },

  // ── Generic ─────────────────────────────────────────────────────────────
  render:           { en: 'Render',           zh: '渲染' },
  loading:          { en: 'Loading…',         zh: '加载中…' },
  error:            { en: 'Error',            zh: '错误' },
  download:         { en: 'Download',         zh: '下载' },
  close:            { en: 'Close',            zh: '关闭' },
  computing:        { en: 'Computing…',       zh: '计算中…' },
}

export function t(key: string): string {
  const entry = dict[key]
  return entry ? entry[lang.value] : key
}

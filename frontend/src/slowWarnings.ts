const SLOW_WARNING_DISABLED_KEY = 'fractal_studio.slow_render_warning_disabled'

export function slowRenderWarningsDisabled(): boolean {
  try {
    return window.localStorage.getItem(SLOW_WARNING_DISABLED_KEY) === '1'
  } catch {
    return false
  }
}

export function promptSlowRenderWarning(message: string): void {
  if (slowRenderWarningsDisabled()) return
  const disable = window.confirm(
    `${message}\n\nOK / 确定: do not remind again / 不再提醒\nCancel / 取消: keep reminding / 继续提醒`,
  )
  if (!disable) return
  try {
    window.localStorage.setItem(SLOW_WARNING_DISABLED_KEY, '1')
  } catch {}
}

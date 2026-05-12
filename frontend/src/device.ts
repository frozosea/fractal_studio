export type DeviceMode = 'desktop' | 'mobile'

const MOBILE_VIEWPORT_WIDTH = 760
const TOUCH_VIEWPORT_WIDTH = 1200

function detectNarrowViewport(): boolean {
  if (typeof window === 'undefined') return false
  return window.innerWidth <= MOBILE_VIEWPORT_WIDTH ||
    window.matchMedia?.(`(max-width: ${MOBILE_VIEWPORT_WIDTH}px)`).matches === true
}

function detectTabletLandscapeViewport(): boolean {
  if (typeof window === 'undefined') return false
  return window.innerWidth > MOBILE_VIEWPORT_WIDTH &&
    window.innerWidth <= TOUCH_VIEWPORT_WIDTH &&
    window.innerWidth > window.innerHeight
}

function detectTouchViewport(): boolean {
  if (typeof window === 'undefined') return false
  const coarsePointer =
    window.matchMedia?.('(pointer: coarse)').matches === true ||
    window.matchMedia?.('(any-pointer: coarse)').matches === true
  const touchPoints = typeof navigator !== 'undefined' && navigator.maxTouchPoints > 0
  return (coarsePointer || touchPoints) && window.innerWidth <= TOUCH_VIEWPORT_WIDTH
}

function detectMobileHardware(): boolean {
  if (typeof navigator === 'undefined') return false

  const nav = navigator as Navigator & {
    userAgentData?: {
      mobile?: boolean
    }
  }

  if (typeof nav.userAgentData?.mobile === 'boolean') {
    return nav.userAgentData.mobile
  }

  const ua = nav.userAgent || ''
  if (/Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini|Mobi/i.test(ua)) {
    return true
  }

  // iPadOS can present itself as "Macintosh" while still behaving like a touch device.
  if (/Macintosh/i.test(ua) && typeof nav.maxTouchPoints === 'number' && nav.maxTouchPoints > 1) {
    return true
  }

  return false
}

function detectDeviceMode(): DeviceMode {
  return detectNarrowViewport() ||
    detectTabletLandscapeViewport() ||
    detectTouchViewport() ||
    detectMobileHardware()
    ? 'mobile'
    : 'desktop'
}

export let deviceMode: DeviceMode = detectDeviceMode()
export let isMobileDevice = deviceMode === 'mobile'

export function applyDeviceModeAttribute() {
  if (typeof document === 'undefined') return
  deviceMode = detectDeviceMode()
  isMobileDevice = deviceMode === 'mobile'
  document.documentElement.setAttribute('data-device', deviceMode)
}

applyDeviceModeAttribute()

if (typeof window !== 'undefined') {
  let resizeTimer: ReturnType<typeof setTimeout> | null = null
  const updateDeviceMode = () => {
    if (resizeTimer) clearTimeout(resizeTimer)
    resizeTimer = setTimeout(applyDeviceModeAttribute, 80)
  }
  window.addEventListener('resize', updateDeviceMode, { passive: true })

  const watchQuery = (query: string) => {
    const media = window.matchMedia?.(query)
    if (media?.addEventListener) {
      media.addEventListener('change', applyDeviceModeAttribute)
    } else {
      media?.addListener?.(applyDeviceModeAttribute)
    }
  }
  watchQuery(`(max-width: ${MOBILE_VIEWPORT_WIDTH}px)`)
  watchQuery(`(min-width: ${MOBILE_VIEWPORT_WIDTH + 1}px) and (max-width: ${TOUCH_VIEWPORT_WIDTH}px) and (orientation: landscape)`)
  watchQuery('(pointer: coarse)')
  watchQuery('(any-pointer: coarse)')
}

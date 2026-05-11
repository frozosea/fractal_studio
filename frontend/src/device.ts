export type DeviceMode = 'desktop' | 'mobile'

function detectMobileDevice(): boolean {
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

export const deviceMode: DeviceMode = detectMobileDevice() ? 'mobile' : 'desktop'
export const isMobileDevice = deviceMode === 'mobile'

export function applyDeviceModeAttribute() {
  if (typeof document === 'undefined') return
  document.documentElement.setAttribute('data-device', deviceMode)
}

applyDeviceModeAttribute()

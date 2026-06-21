<script setup lang="ts">
import { RouterLink } from 'vue-router'
import { t, lang, toggleLang } from '../i18n'
import { isLight, toggleTheme } from '../theme'

defineProps<{ collapsed?: boolean }>()
defineEmits<{ (e: 'toggle'): void }>()

const items = [
  { to: '/',       glyph: 'MP', label: 'nav_map' },
  { to: '/points', glyph: 'PT', label: 'nav_points' },
  { to: '/3d',     glyph: '3D', label: 'nav_3d' },
  { to: '/runs',   glyph: 'RN', label: 'nav_runs' },
  { to: '/system', glyph: 'SY', label: 'nav_system' },
]
</script>

<template>
  <nav :class="['navrail', { collapsed }]">
    <button
      class="rail-toggle"
      :title="collapsed ? 'Expand navigation / 展开导航' : 'Collapse navigation / 折叠导航'"
      @click="$emit('toggle')">
      <span class="toggle-mark">{{ collapsed ? '›' : '‹' }}</span>
    </button>

    <template v-if="!collapsed">
      <div class="brand mono">fs</div>
      <RouterLink
        v-for="item in items"
        :key="item.to"
        :to="item.to"
        class="nav-item"
        active-class="active">
        <span class="glyph mono">{{ item.glyph }}</span>
        <span class="tip">{{ t(item.label) }}</span>
      </RouterLink>

      <div class="spacer"></div>

      <button class="theme-btn nav-item" @click="toggleLang" title="Toggle language / 切换语言">
        <span class="glyph mono">{{ lang === 'en' ? 'ZH' : 'EN' }}</span>
        <span class="tip">{{ lang === 'en' ? '中文' : 'English' }}</span>
      </button>

      <button class="theme-btn nav-item" @click="toggleTheme" :title="isLight ? 'Switch to dark mode / 切换到深色模式' : 'Switch to light mode / 切换到浅色模式'">
        <span class="glyph theme-icon">{{ isLight ? '☀' : '☽' }}</span>
        <span class="tip">{{ isLight ? t('nav_dark') : t('nav_light') }}</span>
      </button>
    </template>
  </nav>
</template>

<style scoped>
.navrail {
  display: flex;
  flex-direction: column;
  align-items: center;
  background: var(--bg);
  padding-top: 8px;
  padding-bottom: 12px;
  gap: 4px;
  height: 100%;
}

.navrail.collapsed {
  padding: 8px 0;
}

.spacer { flex: 1; }

.theme-btn {
  border: none;
  background: transparent;
  cursor: pointer;
  padding: 0;
  text-transform: none;
  letter-spacing: normal;
  font-size: inherit;
}

.theme-btn:hover { color: var(--accent); }

.brand {
  font-family: var(--mono);
  font-size: 14px;
  color: var(--accent);
  margin-top: 6px;
  margin-bottom: 20px;
  letter-spacing: 0.02em;
}

.rail-toggle {
  width: 20px;
  height: 24px;
  min-width: 20px;
  padding: 0;
  border-color: transparent;
  color: var(--text-faint);
}

.rail-toggle:hover {
  border-color: var(--rule-hi);
  color: var(--accent);
}

.toggle-mark {
  display: block;
  font-size: 16px;
  line-height: 1;
  letter-spacing: 0;
}

.nav-item {
  position: relative;
  width: 40px;
  height: 40px;
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--text-faint);
  border: 1px solid transparent;
}

.nav-item:hover { color: var(--text); }

.nav-item.active {
  color: var(--accent);
  border-color: var(--rule-hi);
  background: var(--accent-weak);
}

.glyph {
  font-size: 11px;
  letter-spacing: 0.02em;
  text-transform: uppercase;
}

.theme-icon {
  font-size: 16px;
  letter-spacing: 0;
  text-transform: none;
}

.tip {
  position: absolute;
  left: 46px;
  background: var(--panel);
  border: 1px solid var(--rule);
  padding: 4px 8px;
  font-size: var(--fs-label);
  text-transform: uppercase;
  letter-spacing: 0.08em;
  color: var(--text-dim);
  white-space: nowrap;
  opacity: 0;
  pointer-events: none;
  transition: opacity 0.12s;
  z-index: 20;
}

.nav-item:hover .tip { opacity: 1; }







@media (max-width: 760px), ((pointer: coarse) and (max-width: 1200px)), ((any-pointer: coarse) and (max-width: 1200px)), ((min-width: 761px) and (max-width: 1200px) and (orientation: landscape)) {
  .navrail {
    flex-direction: row;
    align-items: center;
    height: 48px;
    padding: 0 8px;
    gap: 4px;
    border-bottom: 1px solid var(--rule);
    overflow-x: auto;
    overflow-y: hidden;
  }

  .rail-toggle {
    display: none;
  }

  .brand {
    width: 34px;
    flex: 0 0 34px;
    margin: 0 4px 0 0;
    text-align: center;
  }

  .nav-item {
    width: 38px;
    height: 38px;
    flex: 0 0 38px;
  }

  .spacer {
    flex: 1 0 12px;
  }

  .tip {
    display: none;
  }
}
</style>

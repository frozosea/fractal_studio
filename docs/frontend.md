# Frontend Guide / 前端与移动端维护说明

这份文档记录前端结构、状态流和移动端/平板横屏适配策略。它的重点是让后续 UI 改动知道应该改哪里、怎么验证。

Orbit 多步编排、repeat block、自定义公式选择、配方版本和视角存档的产品任务见 [Orbit 编排与配方存档任务清单](orbit_recipe_product_tasks.md)。该编辑器使用 Platform authoring DTO，不应直接让组件拼 Compute v1 JSON。

## App Structure / 应用结构

| Area | Files | Notes |
|---|---|---|
| Shell | `src/App.vue` | 桌面三栏：NavRail / main / StatusRail。移动端改为顶栏 / 内容 / 底部状态栏。 |
| Navigation | `src/router.ts`, `components/NavRail.vue` | 当前页面：Map、Points、3D、Runs、System。 |
| API | `src/api.ts` | 所有后端请求和 TypeScript 类型集中在这里。 |
| Shared state | `src/types.ts`, `src/i18n.ts`, `src/theme.ts` | 全局 status、文案和主题。 |
| Device mode | `src/device.ts` | 设备/viewport 检测，并设置 `<html data-device="...">`。 |
| Base styles | `src/assets/tokens.css`, `src/assets/base.css` | 颜色、spacing、全局控件尺寸、移动端基础规则。 |

## View Responsibilities / 页面职责

| Route | View | Responsibility |
|---|---|---|
| `/` | `views/MapView.vue` | 2D 地图、Julia、特殊点叠加、自定义公式、transition、视频导出。 |
| `/points` | `views/PointsView.vue` | 独立特殊点搜索/枚举工作区。 |
| `/3d` | `views/ThreeDView.vue` | HS field/mesh、transition voxel/mesh、Three.js 预览和导出。 |
| `/runs` | `views/RunsView.vue` | 历史 run、artifact 列表、下载/查看。 |
| `/system` | `views/SystemView.vue` | 硬件、能力探测、benchmark。 |

## Responsive Strategy / 响应式策略

移动端不是单纯按手机 UA 判断，而是合并 viewport、触控和 iPadOS 等信号。

`frontend/src/device.ts` 判为 `mobile` 的情况：

- viewport width `<= 760px`
- touch/coarse pointer 且 width `<= 1200px`
- 平板横屏：width `761px..1200px` 且 `orientation: landscape`
- mobile UA，包含 iPadOS 伪装成 Macintosh 但有多点触控的情况

判定后会设置：

```html
<html data-device="mobile">
```

全局和各 view 的 CSS 使用同一组媒体条件：

```css
@media (max-width: 760px),
  ((pointer: coarse) and (max-width: 1200px)),
  ((any-pointer: coarse) and (max-width: 1200px)),
  ((min-width: 761px) and (max-width: 1200px) and (orientation: landscape)) {
  /* mobile and tablet-landscape layout */
}
```

平板横屏还有单独增强：

```css
@media (min-width: 761px) and (max-width: 1200px) and (orientation: landscape) {
  /* use horizontal space without reverting to desktop side rails */
}
```

## Current Mobile Layout / 当前移动端布局

### App Shell

- Desktop: `NavRail | main | StatusRail`
- Mobile/tablet-touch: `NavRail` 变成 48px 顶部横向导航
- `StatusRail` 变成底部状态栏，默认折叠
- 页面高度使用 `100dvh`，避免移动浏览器地址栏导致 `100vh` 失真

### Map View

- 控制区在手机上横向滚动，避免把画布挤没。
- 平板横屏控制区允许换行，最大高度约束为 `174px`，保留画布空间。
- 手机上 Mandelbrot/Julia 双 pane 上下排列。
- 平板横屏上双 pane 保持左右排列。
- 特殊点 panel 手机上放到底部，平板横屏放回右侧窄栏。
- modal 在小屏下全宽，并限制在 `100dvh` 内滚动。

### 3D View

- 手机上控制区在上、viewer 在下。
- 平板横屏上控制区回到左侧，viewer 占右侧主要空间。
- viewer 使用稳定 `min-height`，避免 Three.js canvas 被压到不可见。

### Runs / System / Points

- 小屏下缩小 padding。
- Runs 表格保留最小宽度，允许局部横向滚动，而不是让整个 app 横向溢出。
- 长 artifact 名称使用换行策略。

## Adding Frontend Features / 新增前端功能

1. 先在 `src/api.ts` 增加请求/响应类型和 client 函数。
2. 页面级工作流写在 `views/*.vue`，可复用交互拆到 `components/`。
3. 会影响全局状态栏的字段，更新 `src/types.ts` 和 `App.vue` 里的默认 status。
4. 新页面需要同步改 `src/router.ts` 和 `components/NavRail.vue`。
5. 有用户可见文本时，检查 `src/i18n.ts`。
6. 新增布局时同时写移动端 media block，尤其是 `MapView.vue`、`ThreeDView.vue` 这类高密度页面。

## Mobile QA Checklist / 移动端检查清单

建议至少检查这些 viewport：

- Phone portrait: `390x844` 或 `412x915`
- Phone landscape: `844x390`
- Tablet landscape: `1024x768`
- Large tablet landscape: `1180x820` 或接近 `1200px` 宽

每次移动端相关改动都检查：

- 顶部导航可以横向滚动，且不会遮住内容。
- StatusRail 在移动端默认折叠，展开后不吃掉全部主内容。
- Map canvas 可见，拖拽/缩放仍然生效。
- 控制区按钮、select、input 高度足够触控。
- 页面整体没有非预期横向滚动；只有 controls/table 等局部区域可以横向滚动。
- 长文件名、长公式、错误信息不会撑破 panel。
- 平板横屏不是桌面三栏老布局，而是顶部导航 + 主内容 + 底部状态栏。

## CSS Pitfalls / CSS 注意事项

- 移动端容器优先用 `minmax(0, 1fr)`、`min-width: 0`、`min-height: 0`，否则 grid/flex 子元素容易把布局撑爆。
- 长文本使用 `overflow-wrap: anywhere` 或局部滚动，不要让整个 app 横向滚动。
- 移动端高度使用 `100dvh`。
- Canvas/Three.js 容器要有稳定尺寸，避免初始化时拿到 0 宽高。
- 对 touch 设备不要只依赖 hover tooltip；关键操作必须有可见按钮或文本。
- 平板横屏不能只看 width；需要结合 `orientation` 和 coarse pointer。

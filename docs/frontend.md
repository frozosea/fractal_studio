# Frontend Guide / 前端与移动端维护说明

这份文档记录前端结构、公开页面、响应式与触控策略。目标是让后续 UI 改动知道应该改哪里、怎么验证。

Orbit 多步编排、repeat block、自定义公式选择、配方版本和视角存档的产品任务见
[Orbit 编排与配方存档任务清单](orbit_recipe_product_tasks.md)。该编辑器使用 Platform authoring
DTO，不应直接让组件拼 Compute v1 JSON。

## Stack / 技术栈

Next.js 14 App Router + React 18 + TypeScript + Tailwind 3.4 + next-intl。
数据层用 TanStack Query 与 zustand，组件基于 Radix primitives，图标用 lucide-react。
包管理器是 pnpm。

| Area | Files | Notes |
|---|---|---|
| Root layout | `src/app/layout.tsx` | `<html>`/`<body>`、`metadata`、`viewport`、全局 provider。 |
| Providers | `src/providers/` | `AuthProvider` 同时负责路由守卫，见下。 |
| API client | `src/lib/api/platform.ts` | 所有 Platform 请求与 TypeScript 类型集中在这里。 |
| i18n | `src/i18n/`, `messages/{zh,en}.json` | `localePrefix: 'as-needed'`，默认 `zh`。 |
| Styles | `src/app/globals.css`, `tailwind.config.ts` | 唯一一份 CSS 文件；设计 token 是 `:root` 上的 HSL 三元组。 |

## Routes / 路由

路由分三组，`(auth)`、`(public)`、`(workbench)`，route group 不影响 URL。

| Group | Routes | Shell |
|---|---|---|
| `(public)` | `/`、`/tutorial`、`/help`、`/creator/[handle]` | `PublicHeader` + `PublicFooter`，`.public-instrument` 坐标纸背景 |
| `(auth)` | `/login`、`/register` | `.auth-instrument` |
| `(workbench)` | `/studio`、`/explore`、`/assets`、`/listings`、`/favorites`、`/purchases`、`/payouts`、`/finance`、`/membership`、`/payment-result`、`/admin` | `WorkbenchShell`（侧边栏 + 顶栏 + 状态条） |

### 公开页面与鉴权守卫

`src/providers/auth-provider.tsx` 挂在根 layout 上，负责把未登录访客送去 `/login`。
允许匿名访问的路径集中在一个地方：

```ts
const PUBLIC_PATHS = new Set(["/", "/tutorial", "/help", "/login", "/register"]);
const PUBLIC_PATH_PREFIXES = ["/creator/"];
export function isPublicPath(pathname: string): boolean { … }
```

**新增公开页面时必须同时更新这个集合**，否则页面会在客户端 hydration 之后被弹回登录页。
`rememberIntendedPath` 也会跳过公开路径：从落地页注册的用户应该进工作台，而不是被送回宣传页。

公开页面尽量写成 server component，这样匿名访客拿到的是真实 HTML，链接分享和抓取才有意义；
只有需要登录态的 `PublicHeader` 是 client component。

后端侧 `GET /v1/explore`、`GET /v1/explore/facets`、`GET /v1/creators/{handle}` 及其 listings
路由都没有鉴权依赖，公开页面因此不需要任何会话。

## Visual language / 视觉语言

代码里有两套语言，不要混用：

- **仪器风格（琥珀色）** — `.public-instrument`、`.auth-instrument`、`.scientific-studio`、
  `.instrument-panel`、`.instrument-kicker`、`.instrument-control`、`.instrument-note`、
  `.instrument-rule`、`.scientific-canvas`。琥珀 `#f0a030`、分隔线 `#2b2f36`、1–2px 圆角、
  等宽大写 kicker、48px 坐标纸。用于公开页面、登录注册、`/studio`、侧边栏与顶栏。
- **深空玻璃（紫青）** — `.fractal-ambient`、`.glass-panel*`、`.gradient-text`、`.btn-glow`、
  `.canvas-glow`，以及 `fractal.*` / `neon.*` / `deep.*` 调色板。`Card` 就是 `.glass-panel`。
  用于会员页与商业化页面。

只有 `:root` 一套 token，没有 `.dark {}` 块——`<html className="dark">` 是写死的，
当前没有浅色主题。

## Responsive strategy / 响应式策略

全部依赖 Tailwind 断点（sm 640 / md 768 / lg 1024 / xl 1280），CSS 里没有手写 `@media`。

### 卡片网格

所有作品网格共用一份 `src/lib/utils/layout.ts` 里的 `CARD_GRID_STYLE`：

```ts
gridTemplateColumns: "repeat(auto-fill, minmax(min(100%, 18rem), 1fr))"
```

`min(100%, 18rem)` 是关键：单写 `minmax(18rem, 1fr)` 在任何窄于 288px 的容器里都会溢出，
而手机减去 padding 之后就是这个宽度。写成 `min(100%, …)` 时最窄会收成一列，然后自己长到
2/3/4 列，不需要断点，也不需要 resize 监听。

配套规则：容器 `min-w-0 overflow-hidden`、文字 `min-w-0` + `truncate`、
图片盒子固定 `aspect-[4/3]` + `object-cover` + `loading="lazy" decoding="async"`。
卡片本身用 `src/components/shared/listing-card.tsx`，市场、收藏、上架、创作者主页共用一份。

### 触控

- `coarse:` 变体（`tailwind.config.ts` 里的 plugin）等价于 `@media (pointer: coarse)`。
  用它把点按目标放大到 44px，而不是全局撑大按钮，否则 studio 的密集控件行会被撑坏。
- `future.hoverOnlyWhenSupported` 已开启：否则触屏上点一下，hover 态会一直粘住。
- 需要**行为**（而不只是样式）随视口变化时，用 `src/lib/hooks/use-media-query.ts` 的
  `useMediaQuery` / `useIsMobile` / `useIsCoarsePointer`。它基于 `useSyncExternalStore`，
  服务端快照恒为 `false`，所以依赖它的分支必须能退化成宽屏布局，否则会 hydration 不一致。

### 视口与安全区

`src/app/layout.tsx` 导出 `viewport`，包含 `viewport-fit: cover` 与 `themeColor`。
**没有**设置 `maximum-scale` / `user-scalable=no`：画布已经用 `touch-action: none` 接管了手势，
再禁用浏览器缩放只会损失可访问性。

因为用了 `viewport-fit: cover`，页面会延伸到刘海和home 指示条下面，所以
`WorkbenchShell`、`main`、`Sidebar`、`PublicHeader`、`PublicFooter` 都要自己补
`env(safe-area-inset-*)`。固定定位的元素拿不到父级 padding，必须单独补。

全屏高度一律用 `dvh`，不要用 `vh`：移动端浏览器工具栏收起时 `100vh` 会超出屏幕。

## Map canvas gestures / 画布手势

`src/components/studio/interactive-fractal-canvas.tsx`。注意它不是 `<canvas>`，
而是一个包着服务端预览 `<img>` 的 `<div>`，拖拽/缩放时先用 CSS transform 给即时反馈。

| 输入 | 行为 |
|---|---|
| 拖拽 / 单指拖拽 | 平移 |
| 滚轮 | 以指针为锚点缩放 |
| 双指捏合 | 以双指中点为锚点缩放，同时可平移 |
| 双击 / 双击屏幕 | 在该点放大一档 |
| 单击（Julia 模式） | 选择复常数 c |

实现要点：

- `pointers` 是一个 `Map<pointerId, {x, y}>`。单个 `drag` 对象无法表达两根手指——第二次
  `pointerdown` 会覆盖第一根的原点，手势会退化成乱跳的平移。
- `spec.scale` 是视口在复平面上的宽度，所以张开手指（ratio > 1）必须**除**以 ratio。
- 锚点从手势开始时锁存的状态解算，而不是上一帧，长手势因此不会累积漂移。
- `gestureWasPinch` 会阻止捏合之后抬手被当成点击，否则 Julia 模式下会误改选中的 c。
- 画布宽度写成 `min(100%, calc(min(64dvh, 52rem) * <aspect>))`：宽屏由高度决定，
  窄屏由容器宽度决定，避免 16:9 画框在手机上要求 900px 宽。

## Adding features / 新增功能

1. 先在 `src/lib/api/platform.ts` 增加请求函数与类型。
2. 页面级工作流写在 `app/[locale]/(group)/…/page.tsx`，可复用交互拆到 `components/`。
3. 新的公开页面要同步更新 `isPublicPath`。
4. 所有用户可见文本都要进 `messages/zh.json` **和** `messages/en.json`，两边键必须一致。
5. 作品网格用 `CARD_GRID_STYLE` + `ListingCard`，不要新写一套列数阶梯。
6. 渲染参数展示用 `components/shared/render-meta.tsx`，标签复用 `studio.variants.*` /
   `studio.colorMaps.*`，不要重复翻译。

## Mobile QA checklist / 移动端检查清单

自动化部分：`pnpm test:e2e` 会跑两个 project，`chromium`（桌面全流程）与 `mobile`
（Pixel 5，只跑 `*.mobile.spec.ts` 的布局检查，包含匿名可访问性与横向溢出断言）。

人工至少检查这些视口：`390x844`、`412x915`、`844x390`（横屏）、`1024x768`、`1180x820`。

每次动布局都检查：

- 页面整体没有横向滚动；只有 facet chip 行、studio 控件行这类局部区域可以横向滚动。
- 侧边栏在窄屏下是抽屉，选中页面后自动收起。
- 画布可见，单指平移、双指缩放、双击放大都生效；Julia 模式下捏合之后不会误选 c。
- 按钮、select、input 在触屏下够大（`coarse:` 变体）。
- 长标题、长公式、长 ID 不会撑破卡片。
- 刘海屏上顶栏内容不被遮挡，底部按钮不被 home 指示条压住。

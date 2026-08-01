import { LocaleSwitcher } from "@/components/layout/locale-switcher";
import { ThemeToggle } from "@/components/layout/theme-toggle";
import { getTranslations } from "next-intl/server";

export default async function AuthLayout({ children }: { children: React.ReactNode }) {
  const t = await getTranslations("auth");
  return (
    <div className="auth-instrument relative min-h-[100dvh] overflow-hidden px-4 py-16 sm:px-8">
      <div className="absolute left-4 top-4 flex items-center gap-3 sm:left-8 sm:top-6">
        <span className="h-2 w-2 bg-amber-400" />
        <span className="font-mono text-[10px] uppercase tracking-[0.2em] text-ink/45">{t("systemLabel")}</span>
      </div>
      <div className="absolute right-4 top-3 flex items-center gap-1 sm:right-8 sm:top-4"><ThemeToggle /><LocaleSwitcher /></div>

      <main className="relative z-10 mx-auto grid min-h-[calc(100dvh-8rem)] max-w-6xl items-center gap-12 lg:grid-cols-[minmax(18rem,1fr)_30rem]">
        <aside className="hidden lg:block">
          <p className="instrument-kicker">{t("planeLabel")}</p>
          <div className="mt-5 max-w-lg border-l border-amber-300/35 pl-6">
            <p className="font-mono text-5xl font-light tracking-[-0.08em] text-ink/80">zₙ₊₁ = zₙ² + c</p>
            <div className="mt-8 grid grid-cols-3 gap-px border border-hairline/10 bg-wash/10 font-mono text-[10px] uppercase tracking-wider text-ink/35">
              <span className="bg-instrument-panel p-3">Re(c)</span>
              <span className="bg-instrument-panel p-3">Im(c)</span>
              <span className="bg-instrument-panel p-3">|z| &gt; 2</span>
            </div>
          </div>
        </aside>
        {children}
      </main>

      <span className="absolute bottom-5 left-8 hidden font-mono text-[9px] uppercase tracking-[0.18em] text-ink/20 sm:block">{t("systemFooter")}</span>
      <span className="absolute bottom-5 right-8 hidden font-mono text-[9px] text-ink/20 sm:block">0.000000 + 0.000000i</span>
    </div>
  );
}

import type { Metadata } from "next";
import { getTranslations } from "next-intl/server";
import {
  ArrowDown,
  ArrowRight,
  Compass,
  Cpu,
  Download,
  Layers,
  Palette,
  Ruler,
  Shuffle,
  Sigma,
  Store,
  Wand2,
} from "lucide-react";
import { Link } from "@/i18n/navigation";
import { Button } from "@/components/ui/button";
import { BUILTIN_VARIANTS, COLOR_MAPS } from "@/lib/studio-catalog";

export async function generateMetadata(): Promise<Metadata> {
  const t = await getTranslations("landing");
  return {
    title: t("meta.title"),
    description: t("meta.description"),
    openGraph: { title: t("meta.title"), description: t("meta.description"), type: "website" },
    twitter: { card: "summary", title: t("meta.title"), description: t("meta.description") },
  };
}

/** Numbered section head, mirroring the studio's `Panel` 01..0N convention. */
function SectionHead({ index, kicker, title }: { index: string; kicker: string; title: string }) {
  return (
    <div className="mb-8">
      <p className="instrument-kicker">
        {index} / {kicker}
      </p>
      <h2 className="mt-3 text-2xl font-light tracking-tight text-white/90 sm:text-3xl">{title}</h2>
      <div className="instrument-rule mt-5" />
    </div>
  );
}

function Section({
  id,
  children,
}: {
  id?: string;
  children: React.ReactNode;
}) {
  return (
    <section id={id} className="mx-auto max-w-6xl px-4 py-14 sm:px-8 sm:py-20">
      {children}
    </section>
  );
}

const FEATURE_ICONS = [Sigma, Layers, Shuffle, Wand2, Palette, Download, Ruler, Cpu] as const;
const FEATURE_KEYS = [
  "variants",
  "julia",
  "transition",
  "formula",
  "coloring",
  "export",
  "precision",
  "cuda",
] as const;

const WORKFLOW_ICONS = [Compass, Wand2, Download, Store] as const;
const WORKFLOW_KEYS = ["explore", "render", "export", "publish"] as const;

export default async function LandingPage() {
  const t = await getTranslations("landing");
  const tAuth = await getTranslations("auth");

  return (
    <>
      {/* ── Hero ───────────────────────────────────────────────────────── */}
      <section className="relative overflow-hidden border-b border-[#2b2f36]">
        <div className="mx-auto max-w-6xl px-4 py-16 sm:px-8 sm:py-24">
          <div className="flex items-center gap-3">
            <span className="h-2 w-2 shrink-0 bg-amber-400" />
            <span className="font-mono text-[10px] uppercase tracking-[0.2em] text-white/45">
              {tAuth("systemLabel")}
            </span>
          </div>

          <p className="instrument-kicker mt-10">{t("hero.kicker")}</p>

          <div className="mt-5 border-l border-amber-300/35 pl-5 sm:pl-6">
            <p className="font-mono text-3xl font-light tracking-[-0.06em] text-white/80 sm:text-5xl">
              zₙ₊₁ = zₙ² + c
            </p>
            <h1 className="mt-6 max-w-3xl text-3xl font-light leading-tight tracking-tight text-white sm:text-5xl">
              {t("hero.title")}
            </h1>
            <p className="mt-5 max-w-2xl text-sm leading-relaxed text-white/55 sm:text-base">
              {t("hero.subtitle")}
            </p>

            <div className="mt-9 flex flex-wrap items-center gap-3">
              <Button asChild size="lg" className="coarse:h-12">
                <Link href="/register">
                  {t("hero.ctaPrimary")}
                  <ArrowRight className="h-4 w-4" />
                </Link>
              </Button>
              <Button asChild size="lg" variant="outline" className="coarse:h-12">
                <Link href="/tutorial">{t("hero.ctaSecondary")}</Link>
              </Button>
            </div>
          </div>

          {/* Spec strip — the same 1px-gap grid the sign-in aside uses. */}
          <div className="mt-14 grid max-w-lg grid-cols-3 gap-px border border-white/10 bg-white/10 font-mono text-[10px] uppercase tracking-wider text-white/35">
            <span className="bg-[#0b0d10] p-3">Re(c)</span>
            <span className="bg-[#0b0d10] p-3">Im(c)</span>
            <span className="bg-[#0b0d10] p-3">|z| &gt; 2</span>
          </div>

          <p className="mt-12 flex items-center gap-2 font-mono text-[10px] uppercase tracking-[0.18em] text-white/30">
            <ArrowDown className="h-3.5 w-3.5" aria-hidden="true" />
            {t("hero.scroll")}
          </p>
        </div>
      </section>

      {/* ── 01 / Introduction ──────────────────────────────────────────── */}
      <Section id="intro">
        <SectionHead index="01" kicker={t("intro.kicker")} title={t("intro.title")} />
        <div className="grid gap-8 lg:grid-cols-[minmax(0,1fr)_minmax(0,1fr)] lg:gap-12">
          <div className="space-y-4 text-sm leading-relaxed text-white/60">
            <p>{t("intro.body1")}</p>
            <p>{t("intro.body2")}</p>
          </div>
          <ul className="space-y-px bg-white/10">
            {(["browser", "platform", "compute"] as const).map((layer, index) => (
              <li key={layer} className="bg-[#0b0d10] p-4 sm:p-5">
                <div className="flex items-baseline gap-3">
                  <span className="font-mono text-[10px] text-amber-400/70">
                    {String(index + 1).padStart(2, "0")}
                  </span>
                  <div className="min-w-0">
                    <p className="text-sm text-white/85">{t(`intro.layers.${layer}.name`)}</p>
                    <p className="mt-1 text-xs leading-relaxed text-white/40">
                      {t(`intro.layers.${layer}.note`)}
                    </p>
                  </div>
                </div>
              </li>
            ))}
          </ul>
        </div>
      </Section>

      {/* ── 02 / Capabilities ──────────────────────────────────────────── */}
      <Section id="features">
        <SectionHead index="02" kicker={t("features.kicker")} title={t("features.title")} />
        <div className="grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
          {FEATURE_KEYS.map((key, index) => {
            const Icon = FEATURE_ICONS[index]!;
            return (
              <div key={key} className="instrument-panel min-w-0 p-4 sm:p-5">
                <Icon className="h-4 w-4 text-amber-400/80" aria-hidden="true" />
                <p className="mt-3 text-sm text-white/85">
                  {t(`features.items.${key}.name`, {
                    variants: BUILTIN_VARIANTS.length,
                    colorMaps: COLOR_MAPS.length,
                  })}
                </p>
                <p className="mt-2 text-xs leading-relaxed text-white/40">
                  {t(`features.items.${key}.note`)}
                </p>
              </div>
            );
          })}
        </div>
      </Section>

      {/* ── 03 / Workflow ──────────────────────────────────────────────── */}
      <Section id="workflow">
        <SectionHead index="03" kicker={t("workflow.kicker")} title={t("workflow.title")} />
        <ol className="grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
          {WORKFLOW_KEYS.map((key, index) => {
            const Icon = WORKFLOW_ICONS[index]!;
            return (
              <li key={key} className="instrument-panel min-w-0 p-4 sm:p-5">
                <div className="flex items-center justify-between gap-2">
                  <Icon className="h-4 w-4 text-amber-400/80" aria-hidden="true" />
                  <span className="font-mono text-[10px] text-white/25">
                    {String(index + 1).padStart(2, "0")}
                  </span>
                </div>
                <p className="mt-3 text-sm text-white/85">{t(`workflow.steps.${key}.name`)}</p>
                <p className="mt-2 text-xs leading-relaxed text-white/40">
                  {t(`workflow.steps.${key}.note`)}
                </p>
              </li>
            );
          })}
        </ol>
        <Button asChild variant="ghost" size="sm" className="mt-6 coarse:h-10">
          <Link href="/tutorial">
            {t("workflow.cta")}
            <ArrowRight className="h-3.5 w-3.5" />
          </Link>
        </Button>
      </Section>

      {/* ── 04 / Marketplace ──────────────────────────────────────────── */}
      <Section id="market">
        <SectionHead index="04" kicker={t("market.kicker")} title={t("market.title")} />
        <div className="grid gap-8 lg:grid-cols-[minmax(0,1.1fr)_minmax(0,1fr)] lg:gap-12">
          <p className="text-sm leading-relaxed text-white/60">{t("market.body")}</p>
          <ul className="space-y-2.5">
            {(["discover", "buy", "earn"] as const).map((point) => (
              <li key={point} className="flex items-start gap-3 text-sm text-white/70">
                <span className="mt-1.5 h-1.5 w-1.5 shrink-0 bg-amber-400/70" />
                <span className="min-w-0">{t(`market.points.${point}`)}</span>
              </li>
            ))}
          </ul>
        </div>
      </Section>

      {/* ── 05 / Membership ───────────────────────────────────────────── */}
      <Section id="membership">
        <SectionHead index="05" kicker={t("membership.kicker")} title={t("membership.title")} />
        <div className="instrument-panel flex flex-col gap-6 p-5 sm:flex-row sm:items-center sm:justify-between sm:p-7">
          <div className="min-w-0">
            <div className="flex items-baseline gap-2">
              <span className="font-mono text-4xl font-light text-amber-300">¥29</span>
              <span className="font-mono text-[10px] uppercase tracking-[0.18em] text-white/35">
                {t("membership.priceNote")}
              </span>
            </div>
            <p className="mt-3 max-w-xl text-sm leading-relaxed text-white/55">
              {t("membership.body")}
            </p>
          </div>
          <Button asChild size="lg" variant="outline" className="shrink-0 coarse:h-12">
            <Link href="/register">{t("membership.cta")}</Link>
          </Button>
        </div>
      </Section>

      {/* ── Closing CTA ───────────────────────────────────────────────── */}
      <section className="border-t border-[#2b2f36] bg-[#0b0d10]">
        <div className="mx-auto max-w-6xl px-4 py-14 text-center sm:px-8 sm:py-20">
          <p className="instrument-kicker">{t("cta.kicker")}</p>
          <h2 className="mx-auto mt-4 max-w-2xl text-2xl font-light tracking-tight text-white sm:text-3xl">
            {t("cta.title")}
          </h2>
          <p className="mx-auto mt-4 max-w-xl text-sm leading-relaxed text-white/50">
            {t("cta.body")}
          </p>
          <div className="mt-8 flex flex-wrap items-center justify-center gap-3">
            <Button asChild size="lg" className="coarse:h-12">
              <Link href="/register">
                {t("cta.primary")}
                <ArrowRight className="h-4 w-4" />
              </Link>
            </Button>
            <Button asChild size="lg" variant="ghost" className="coarse:h-12">
              <Link href="/help">{t("cta.help")}</Link>
            </Button>
          </div>
        </div>
      </section>
    </>
  );
}

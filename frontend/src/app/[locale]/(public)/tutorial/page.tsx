import type { Metadata } from "next";
import { getTranslations } from "next-intl/server";
import { ArrowRight } from "lucide-react";
import { Link } from "@/i18n/navigation";
import { Button } from "@/components/ui/button";
import { BUILTIN_VARIANTS, COLOR_MAPS, MAX_OUTPUT_EDGE } from "@/lib/studio-catalog";

export async function generateMetadata(): Promise<Metadata> {
  const t = await getTranslations("tutorial");
  return {
    title: t("meta.title"),
    description: t("meta.description"),
    openGraph: { title: t("meta.title"), description: t("meta.description"), type: "article" },
  };
}

const STEPS = ["mode", "navigate", "julia", "depth", "coloring", "publish"] as const;

/** Gesture reference for step 02 — the one place a table beats prose. */
const GESTURES = ["drag", "wheel", "pinch", "doubleTap", "buttons"] as const;

export default async function TutorialPage() {
  const t = await getTranslations("tutorial");

  return (
    <div className="mx-auto max-w-4xl px-4 py-14 sm:px-8 sm:py-20">
      <p className="instrument-kicker">{t("kicker")}</p>
      <h1 className="mt-3 text-3xl font-light tracking-tight text-ink sm:text-4xl">
        {t("title")}
      </h1>
      <p className="mt-5 max-w-2xl text-sm leading-relaxed text-ink/55">{t("lede")}</p>
      <div className="instrument-rule mt-8" />

      {/* Contents — anchors, so a long guide stays navigable on a phone. */}
      <nav className="mt-8" aria-label={t("contents")}>
        <p className="font-mono text-[10px] uppercase tracking-[0.18em] text-ink/30">
          {t("contents")}
        </p>
        <ol className="mt-3 space-y-1.5">
          {STEPS.map((step, index) => (
            <li key={step}>
              <a
                href={`#step-${index + 1}`}
                className="group flex items-baseline gap-3 text-sm text-ink/50 transition-colors hover:text-ink/85"
              >
                <span className="font-mono text-[11px] text-amber-400/60">
                  {String(index + 1).padStart(2, "0")}
                </span>
                <span className="min-w-0">{t(`steps.${step}.name`)}</span>
              </a>
            </li>
          ))}
        </ol>
      </nav>

      <div className="mt-14 space-y-14">
        {STEPS.map((step, index) => (
          <section key={step} id={`step-${index + 1}`} className="scroll-mt-20">
            <p className="instrument-kicker">
              {String(index + 1).padStart(2, "0")} / {t(`steps.${step}.kicker`)}
            </p>
            <h2 className="mt-3 text-xl font-light tracking-tight text-ink/90 sm:text-2xl">
              {t(`steps.${step}.name`)}
            </h2>
            <div className="mt-5 space-y-4 text-sm leading-relaxed text-ink/60">
              <p>{t(`steps.${step}.body1`, { variants: BUILTIN_VARIANTS.length, colorMaps: COLOR_MAPS.length, maxEdge: MAX_OUTPUT_EDGE })}</p>
              <p>{t(`steps.${step}.body2`, { variants: BUILTIN_VARIANTS.length, colorMaps: COLOR_MAPS.length, maxEdge: MAX_OUTPUT_EDGE })}</p>
            </div>

            {step === "navigate" && (
              <div className="mt-6 grid gap-px bg-wash/10 sm:grid-cols-2">
                {GESTURES.map((gesture) => (
                  <div key={gesture} className="min-w-0 bg-instrument-panel p-4">
                    <p className="font-mono text-[10px] uppercase tracking-[0.16em] text-amber-200/70">
                      {t(`gestures.${gesture}.input`)}
                    </p>
                    <p className="mt-2 text-sm text-ink/70">{t(`gestures.${gesture}.effect`)}</p>
                  </div>
                ))}
              </div>
            )}

            <p className="instrument-note">{t(`steps.${step}.tip`)}</p>
          </section>
        ))}
      </div>

      <div className="instrument-rule mt-16" />
      <div className="mt-8 flex flex-wrap items-center gap-3">
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
  );
}

import type { Metadata } from "next";
import { getTranslations } from "next-intl/server";
import { ChevronRight } from "lucide-react";
import { Link } from "@/i18n/navigation";
import { Button } from "@/components/ui/button";
import { MAX_OUTPUT_EDGE } from "@/lib/studio-catalog";

export async function generateMetadata(): Promise<Metadata> {
  const t = await getTranslations("help");
  return {
    title: t("meta.title"),
    description: t("meta.description"),
    openGraph: { title: t("meta.title"), description: t("meta.description"), type: "article" },
  };
}

/**
 * Question ids per group. Native <details> keeps this zero-JS and keyboard
 * accessible, matching the disclosure the studio already uses for its formula
 * examples.
 */
const GROUPS = [
  { id: "account", questions: ["password", "session", "reset"] },
  { id: "membership", questions: ["what", "pay", "pending", "quota"] },
  { id: "render", questions: ["slow", "size", "brightness", "deep"] },
  { id: "market", questions: ["become", "handle", "publish", "payout"] },
  { id: "mobile", questions: ["zoom", "nav", "canvas"] },
  { id: "trouble", questions: ["noPreview", "noPayment", "tabs"] },
] as const;

export default async function HelpPage() {
  const t = await getTranslations("help");

  return (
    <div className="mx-auto max-w-4xl px-4 py-14 sm:px-8 sm:py-20">
      <p className="instrument-kicker">{t("kicker")}</p>
      <h1 className="mt-3 text-3xl font-light tracking-tight text-ink sm:text-4xl">
        {t("title")}
      </h1>
      <p className="mt-5 max-w-2xl text-sm leading-relaxed text-ink/55">{t("lede")}</p>
      <div className="instrument-rule mt-8" />

      <nav className="mt-8 flex flex-wrap gap-x-5 gap-y-2" aria-label={t("contents")}>
        {GROUPS.map((group, index) => (
          <a
            key={group.id}
            href={`#group-${group.id}`}
            className="text-sm text-ink/60 transition-colors hover:text-ink/80"
          >
            <span className="font-mono text-[11px] text-amber-400/60">
              {String(index + 1).padStart(2, "0")}
            </span>{" "}
            {t(`groups.${group.id}.name`)}
          </a>
        ))}
      </nav>

      <div className="mt-12 space-y-12">
        {GROUPS.map((group, index) => (
          <section key={group.id} id={`group-${group.id}`} className="scroll-mt-20">
            <p className="instrument-kicker">
              {String(index + 1).padStart(2, "0")} / {t(`groups.${group.id}.name`)}
            </p>

            <div className="mt-4 space-y-px bg-wash/10">
              {group.questions.map((question) => (
                <details key={question} className="group min-w-0 bg-instrument-panel">
                  <summary className="flex cursor-pointer list-none items-start gap-3 p-4 text-sm text-ink/80 transition-colors hover:text-ink coarse:py-5">
                    <ChevronRight
                      className="mt-0.5 h-4 w-4 shrink-0 text-amber-400/60 transition-transform duration-150 group-open:rotate-90"
                      aria-hidden="true"
                    />
                    <span className="min-w-0">{t(`groups.${group.id}.items.${question}.q`)}</span>
                  </summary>
                  <div className="px-4 pb-4 pl-11 text-sm leading-relaxed text-ink/55">
                    {t(`groups.${group.id}.items.${question}.a`, { maxEdge: MAX_OUTPUT_EDGE })}
                  </div>
                </details>
              ))}
            </div>
          </section>
        ))}
      </div>

      <div className="instrument-rule mt-16" />
      <div className="mt-8">
        <p className="text-sm text-ink/55">{t("more.body")}</p>
        <div className="mt-5 flex flex-wrap items-center gap-3">
          <Button asChild variant="outline" className="coarse:h-12">
            <Link href="/tutorial">{t("more.tutorial")}</Link>
          </Button>
          <Button asChild variant="ghost" className="coarse:h-12">
            <Link href="/register">{t("more.signUp")}</Link>
          </Button>
        </div>
      </div>
    </div>
  );
}

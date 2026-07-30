import { getTranslations } from "next-intl/server";
import { Link } from "@/i18n/navigation";

export async function PublicFooter() {
  const t = await getTranslations("landing");
  const tAuth = await getTranslations("auth");

  return (
    <footer
      className="border-t border-[#2b2f36] bg-[#090a0c]"
      style={{ paddingBottom: "env(safe-area-inset-bottom)" }}
    >
      <div className="mx-auto flex max-w-6xl flex-col gap-4 px-4 py-8 sm:px-8 sm:py-10 md:flex-row md:items-center md:justify-between">
        <div className="flex items-center gap-3">
          <span className="h-2 w-2 shrink-0 bg-amber-400" />
          <span className="font-mono text-[10px] uppercase tracking-[0.2em] text-white/40">
            {tAuth("systemFooter")}
          </span>
        </div>

        <nav className="flex flex-wrap items-center gap-x-5 gap-y-2" aria-label={t("nav.label")}>
          <Link href="/" className="text-xs text-white/45 transition-colors hover:text-white/75">
            {t("nav.home")}
          </Link>
          <Link href="/tutorial" className="text-xs text-white/45 transition-colors hover:text-white/75">
            {t("nav.tutorial")}
          </Link>
          <Link href="/help" className="text-xs text-white/45 transition-colors hover:text-white/75">
            {t("nav.help")}
          </Link>
          <Link href="/register" className="text-xs text-amber-400/80 transition-colors hover:text-amber-300">
            {t("nav.signUp")}
          </Link>
        </nav>

        <span className="font-mono text-[10px] uppercase tracking-[0.18em] text-white/25">
          {t("footer.licence")}
        </span>
      </div>
    </footer>
  );
}

import { getTranslations } from "next-intl/server";
import { Link } from "@/i18n/navigation";
import { Button } from "@/components/ui/button";

export default async function NotFound() {
  const t = await getTranslations("common.notFound");

  return (
    <div className="public-instrument flex min-h-[100dvh] flex-col items-center justify-center px-4 text-center">
      <p className="font-mono text-6xl font-light tracking-[-0.06em] text-amber-400/80">404</p>
      <h1 className="mt-6 text-xl font-light tracking-tight text-white/90">{t("title")}</h1>
      <p className="mt-3 max-w-md text-sm leading-relaxed text-white/50">{t("description")}</p>
      <div className="mt-8 flex flex-wrap items-center justify-center gap-3">
        <Button asChild className="coarse:h-12">
          <Link href="/">{t("home")}</Link>
        </Button>
        <Button asChild variant="ghost" className="coarse:h-12">
          <Link href="/help">{t("help")}</Link>
        </Button>
      </div>
    </div>
  );
}

import { redirect } from "@/i18n/navigation";
import { routing } from "@/i18n/routing";

export default async function HomePage({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  const requestedLocale = routing.locales.includes(locale as "zh" | "en")
    ? locale as "zh" | "en"
    : routing.defaultLocale;
  return redirect({ href: "/studio", locale: requestedLocale });
}

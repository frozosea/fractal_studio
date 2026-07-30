import { redirect } from "@/i18n/navigation";
import { routing } from "@/i18n/routing";

export default async function HomePage({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  const requestedLocale = routing.locales.includes(locale as "zh" | "en")
    ? locale as "zh" | "en"
    : routing.defaultLocale;
  // The public entry point must never render the protected workbench first.
  // AuthProvider moves an already authenticated session on to its landing
  // page; anonymous visitors remain on the sign-in screen from the first
  // server response, without a Studio flash.
  return redirect({ href: "/login", locale: requestedLocale });
}

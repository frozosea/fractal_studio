import type { Metadata, Viewport } from "next";
import { NextIntlClientProvider } from "next-intl";
import { getLocale, getMessages } from "next-intl/server";
import { Providers } from "@/providers/providers";
import { THEME_INIT_SCRIPT } from "@/lib/theme";
import "./globals.css";

export const metadata: Metadata = {
  title: "Fractal Studio",
  description: "Fractal art exploration and rendering platform",
};

export const viewport: Viewport = {
  width: "device-width",
  initialScale: 1,
  // Notched devices report their safe areas only under `cover`, which the app
  // shell and the public chrome pad against.
  viewportFit: "cover",
  // Follows the OS rather than an in-app override: the browser reads this from
  // static metadata before any of our JS runs, so it cannot see the stored
  // preference. Worst case the chrome around the page is a shade off until the
  // next load, which is far cheaper than blocking paint on it.
  themeColor: [
    { media: "(prefers-color-scheme: light)", color: "#faf9f7" },
    { media: "(prefers-color-scheme: dark)", color: "#0a0b0d" },
  ],
  // Deliberately no `maximum-scale`/`user-scalable=no`: the map canvas already
  // claims its own gestures through `touch-action: none`, so disabling browser
  // zoom would only cost accessibility elsewhere.
};

export default async function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  const locale = await getLocale();
  const messages = await getMessages();

  return (
    // The theme class is written by THEME_INIT_SCRIPT before first paint, so
    // the server cannot predict it; suppressHydrationWarning covers exactly
    // that one attribute mismatch on <html>.
    <html lang={locale} dir="ltr" suppressHydrationWarning>
      <head>
        <script dangerouslySetInnerHTML={{ __html: THEME_INIT_SCRIPT }} />
      </head>
      <body className="min-h-[100dvh] bg-background text-foreground antialiased fractal-ambient">
        <NextIntlClientProvider messages={messages}>
          <Providers>{children}</Providers>
        </NextIntlClientProvider>
      </body>
    </html>
  );
}

import type { Metadata, Viewport } from "next";
import { NextIntlClientProvider } from "next-intl";
import { getLocale, getMessages } from "next-intl/server";
import { Providers } from "@/providers/providers";
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
  themeColor: "#0a0b0d",
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
    <html lang={locale} dir="ltr" className="dark">
      <body className="min-h-[100dvh] bg-background text-foreground antialiased fractal-ambient">
        <NextIntlClientProvider messages={messages}>
          <Providers>{children}</Providers>
        </NextIntlClientProvider>
      </body>
    </html>
  );
}

import createMiddleware from "next-intl/middleware";
import { routing } from "@/i18n/routing";

export default createMiddleware(routing);

export const config = {
  // Platform reverse-proxy routes are transport, never locale pages.
  matcher: "/((?!api|platform|_next|_vercel|.*\\..*).*)",
};

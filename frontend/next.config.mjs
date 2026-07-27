import createNextIntlPlugin from "next-intl/plugin";

const withNextIntl = createNextIntlPlugin();

/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  poweredByHeader: false,
  async rewrites() {
    return [
      {
        source: "/platform/:path*",
        destination: `${process.env.PLATFORM_INTERNAL_URL ?? "http://localhost:8000"}/:path*`,
      },
    ];
  },
};

export default withNextIntl(nextConfig);

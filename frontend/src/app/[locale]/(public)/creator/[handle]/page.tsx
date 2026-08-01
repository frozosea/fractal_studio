import type { Metadata } from "next";
import { getTranslations } from "next-intl/server";
import { notFound } from "next/navigation";
import { CreatorGallery } from "./gallery";

/** Mirrors the handle format enforced by Pydantic and a DB check constraint. */
const HANDLE_PATTERN = /^[a-z0-9_]{3,32}$/;

type Params = { params: Promise<{ handle: string }> };

async function fetchProfile(handle: string) {
  if (!HANDLE_PATTERN.test(handle)) return null;
  // Server-rendered so a shared link is meaningful to crawlers and to visitors
  // who are not signed in; the endpoint is public for the same reason.
  const base = process.env.PLATFORM_INTERNAL_URL ?? "http://localhost:8000";
  const response = await fetch(`${base}/v1/creators/${encodeURIComponent(handle)}`, {
    headers: { Accept: "application/json" },
    cache: "no-store",
  });
  if (!response.ok) return null;
  const body = (await response.json()) as { data: { handle: string; displayName: string; publishedCount: number } };
  return body.data;
}

export async function generateMetadata({ params }: Params): Promise<Metadata> {
  const { handle } = await params;
  const profile = await fetchProfile(handle);
  const t = await getTranslations("creator");
  if (!profile) return { title: t("notFound.title") };
  const title = t("meta.title", { name: profile.displayName });
  const description = t("meta.description", { name: profile.displayName, count: profile.publishedCount });
  return { title, description, openGraph: { title, description, type: "profile" } };
}

export default async function CreatorPage({ params }: Params) {
  const { handle } = await params;
  const profile = await fetchProfile(handle);
  if (!profile) notFound();
  const t = await getTranslations("creator");

  // No avatar exists in the data model, so a monogram from the display name
  // stands in rather than inventing an upload path for it.
  const monogram = [...profile.displayName.trim()][0]?.toUpperCase() ?? "?";

  return (
    <div className="mx-auto max-w-6xl px-4 py-12 sm:px-8 sm:py-16">
      <div className="flex flex-wrap items-center gap-5">
        <span
          aria-hidden="true"
          className="flex h-16 w-16 shrink-0 items-center justify-center border border-amber-300/30 bg-instrument-panel font-mono text-2xl text-amber-300/80"
        >
          {monogram}
        </span>
        <div className="min-w-0">
          <h1 className="truncate text-2xl font-light tracking-tight text-ink sm:text-3xl">
            {profile.displayName}
          </h1>
          <p className="mt-1 font-mono text-[11px] uppercase tracking-[0.16em] text-ink/35">
            @{profile.handle} · {t("published", { count: profile.publishedCount })}
          </p>
        </div>
      </div>

      <div className="instrument-rule mt-8" />

      <CreatorGallery handle={profile.handle} />
    </div>
  );
}

import type { ReactNode } from "react";
import { cn } from "@/lib/utils/cn";

interface ListingCardProps {
  title: ReactNode;
  /** One line under the title — creator, price, status. */
  subtitle?: ReactNode;
  previewUrl?: string | null;
  previewAlt: string;
  /** Shown in place of the image while a preview is still being generated. */
  previewFallback: ReactNode;
  /** Sits opposite the title, for a single compact control such as a favourite toggle. */
  headerAction?: ReactNode;
  /** Body content below the header: description, metadata, action buttons. */
  children?: ReactNode;
  className?: string;
}

/**
 * The gallery card used by the marketplace, favourites, listings and creator
 * pages. Centralising it means the overflow rules that keep a long title or a
 * wide preview from bursting a narrow grid track are written once:
 * `min-w-0` defeats a grid item's automatic minimum width, `truncate` bounds the
 * title, and the fixed `aspect-[4/3]` box stops the layout shifting as columns
 * reflow.
 */
export function ListingCard({
  title,
  subtitle,
  previewUrl,
  previewAlt,
  previewFallback,
  headerAction,
  children,
  className,
}: ListingCardProps) {
  return (
    <article className={cn("min-w-0 overflow-hidden rounded-xl border border-white/10", className)}>
      <div className="aspect-[4/3] bg-white/5">
        {previewUrl ? (
          <img
            src={previewUrl}
            alt={previewAlt}
            loading="lazy"
            decoding="async"
            className="block h-full w-full object-cover"
          />
        ) : (
          <div className="flex h-full items-center justify-center p-3 text-center text-sm text-muted-foreground">
            {previewFallback}
          </div>
        )}
      </div>
      <div className="space-y-3 p-4">
        <div className="flex items-start justify-between gap-3">
          <div className="min-w-0">
            <h2 className="truncate font-medium">{title}</h2>
            {subtitle && <p className="truncate text-sm text-muted-foreground">{subtitle}</p>}
          </div>
          {headerAction}
        </div>
        {children}
      </div>
    </article>
  );
}

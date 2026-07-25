"use client";

import { Button } from "@/components/ui/button";

export default function ErrorPage({
  error,
  reset,
}: {
  error: Error & { digest?: string };
  reset: () => void;
}) {
  return (
    <div className="flex min-h-screen flex-col items-center justify-center bg-deep-void">
      <h1 className="text-4xl font-bold text-destructive">Error</h1>
      <p className="mt-4 max-w-md text-center text-muted-foreground">
        {error.message || "Something went wrong"}
      </p>
      <Button onClick={reset} variant="fractal" className="mt-8">
        Try again
      </Button>
    </div>
  );
}

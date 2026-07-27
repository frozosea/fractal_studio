import Link from "next/link";

export default function NotFound() {
  return (
    <div className="flex min-h-screen flex-col items-center justify-center bg-deep-void">
      <h1 className="text-6xl font-bold text-fractal-500">404</h1>
      <p className="mt-4 text-lg text-muted-foreground">Page not found</p>
      <Link
        href="/studio"
        className="mt-8 rounded-lg bg-fractal-600 px-6 py-2 text-white hover:bg-fractal-700"
      >
        Back to Studio
      </Link>
    </div>
  );
}

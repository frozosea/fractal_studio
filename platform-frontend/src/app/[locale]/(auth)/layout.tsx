export default function AuthLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return (
    <div
      className="flex min-h-screen items-center justify-center px-4 fractal-ambient"
      style={{
        background:
          "radial-gradient(ellipse 60% 50% at 40% 30%, hsl(271 60% 25% / 0.10) 0%, transparent 60%), " +
          "radial-gradient(ellipse 50% 60% at 60% 70%, hsl(178 60% 20% / 0.08) 0%, transparent 60%), " +
          "radial-gradient(ellipse 100% 100% at 50% 50%, hsl(228 50% 6%) 0%, hsl(228 50% 3%) 100%)",
      }}
    >
      {children}
    </div>
  );
}

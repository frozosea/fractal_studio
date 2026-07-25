"use client";

interface ProgressBarProps {
  percent: number;
}

export function ProgressBar({ percent }: ProgressBarProps) {
  const clamped = Math.max(0, Math.min(100, percent));

  return (
    <div
      className="h-1.5 w-full overflow-hidden rounded-full"
      style={{ background: "hsl(226 22% 14%)" }}
    >
      <div
        className="h-full rounded-full transition-all duration-700 ease-out"
        style={{
          width: `${clamped}%`,
          background:
            "linear-gradient(90deg, hsl(271 85% 50%) 0%, hsl(178 75% 50%) 100%)",
          boxShadow: "0 0 8px hsl(271 85% 50% / 0.2)",
        }}
      />
    </div>
  );
}

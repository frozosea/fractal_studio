"use client";
import * as React from "react";
import { Badge } from "@/components/ui/badge";
import { Cpu, ListTodo, Film } from "lucide-react";

interface HardwareInfo {
  cpu?: string;
  cores?: number;
  memory?: string;
}

export function StatusRail() {
  const [hardware, setHardware] = React.useState<HardwareInfo | null>(null);
  const [activeTasks, setActiveTasks] = React.useState<number>(0);
  const [renderStatus] = React.useState<"idle" | "rendering" | "error">("idle");

  React.useEffect(() => {
    let mounted = true;

    const fetchHardware = async () => {
      try {
        const res = await fetch("/api/system/hardware");
        if (res.ok) {
          const data = await res.json();
          if (mounted) setHardware(data);
        }
      } catch {
        // silently fail
      }
    };

    const fetchActiveTasks = async () => {
      try {
        const res = await fetch("/api/tasks/active");
        if (res.ok) {
          const data = await res.json();
          if (mounted) setActiveTasks(Array.isArray(data) ? data.length : data.count ?? 0);
        }
      } catch {
        // silently fail
      }
    };

    fetchHardware();
    fetchActiveTasks();

    const taskInterval = setInterval(fetchActiveTasks, 10000);
    return () => {
      mounted = false;
      clearInterval(taskInterval);
    };
  }, []);

  return (
    <div className="flex h-8 shrink-0 items-center gap-3 border-b border-white/5 bg-deep-void/60 px-4">
      {/* Hardware info */}
      {hardware && (
        <div className="flex items-center gap-1.5">
          <Cpu className="h-3 w-3 text-muted-foreground" />
          <span className="font-mono text-[11px] text-muted-foreground">
            {hardware.cpu ?? "CPU"} {hardware.cores ? `(${hardware.cores}c)` : ""}
            {hardware.memory ? ` | ${hardware.memory}` : ""}
          </span>
        </div>
      )}

      <div className="h-3 w-px bg-white/5" />

      {/* Active tasks */}
      <div className="flex items-center gap-1.5">
        <ListTodo className="h-3 w-3 text-muted-foreground" />
        <span className="font-mono text-[11px] text-muted-foreground">
          {activeTasks} active task{activeTasks !== 1 ? "s" : ""}
        </span>
      </div>

      <div className="h-3 w-px bg-white/5" />

      {/* Render engine status */}
      <div className="flex items-center gap-1.5">
        <Film className="h-3 w-3 text-muted-foreground" />
        <Badge
          variant={
            renderStatus === "rendering"
              ? "running"
              : renderStatus === "error"
              ? "error"
              : "outline"
          }
          className="h-5 px-1.5 text-[10px] font-mono"
        >
          {renderStatus === "idle" && "Render Idle"}
          {renderStatus === "rendering" && "Rendering"}
          {renderStatus === "error" && "Render Error"}
        </Badge>
      </div>

      <div className="flex-1" />

      {/* Version */}
      <span className="font-mono text-[10px] text-muted-foreground/50">v1.0.0</span>
    </div>
  );
}

"use client";

import { QueryProvider } from "./query-provider";
import { AuthProvider } from "./auth-provider";
import { RequestActivityIndicator } from "@/components/shared/request-activity-indicator";
import { Toaster } from "@/components/ui/toaster";

export function Providers({ children }: { children: React.ReactNode }) {
  return (
    <QueryProvider>
      <AuthProvider>
        {children}
        <RequestActivityIndicator />
        <Toaster />
      </AuthProvider>
    </QueryProvider>
  );
}

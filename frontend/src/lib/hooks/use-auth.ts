"use client";

import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { useAuthStore } from "@/stores/auth-store";
import type { CredentialsInput, UserView } from "@/types/auth";
import { useEffect, useRef } from "react";
import { useTranslations } from "next-intl";
import { toast } from "@/components/ui/toaster";
import { platform, PlatformApiError, resetPlatformClientState, type PlatformUser } from "@/lib/api/platform";

export const authKeys = {
  me: ["me"] as const,
};

const asUser = (user: PlatformUser): UserView => user;

const currentUser = async (): Promise<UserView | null> => {
  try {
    return asUser(await platform.auth.me());
  } catch (error) {
    if (error instanceof PlatformApiError && error.status === 401) return null;
    throw error;
  }
};

// Sync hook: on mount, load from localStorage
export function useCurrentUser() {
  return useQuery({
    queryKey: authKeys.me,
    queryFn: async () => {
      return currentUser();
    },
    retry: 0,
    // The session lives in a cookie, which every tab in the window shares:
    // signing in elsewhere silently re-points this tab at another account.
    // Never serve a cached identity — revalidate on mount and on every focus
    // so a stale tab corrects itself instead of rendering as the wrong user.
    staleTime: 0,
    gcTime: 30 * 60_000,
    refetchOnWindowFocus: "always",
    refetchOnMount: "always",
  });
}

export function useSyncAuth() {
  const { data: user, isPending } = useCurrentUser();
  const { setUser, clearUser, setPending } = useAuthStore();
  const qc = useQueryClient();
  const t = useTranslations("auth");
  const lastUserId = useRef<string | null>(null);

  // Drop every cached response when the signed-in identity changes, so one
  // account never renders data fetched for another (e.g. a browser tab left
  // open across a logout, or a sign-in performed in a sibling tab).
  useEffect(() => {
    if (isPending) return;
    const id = user?.id ?? null;
    const previous = lastUserId.current;
    if (previous !== id) {
      resetPlatformClientState(id);
      qc.removeQueries({ predicate: (query) => query.queryKey[0] !== authKeys.me[0] });
      // Announce only a switch away from an established identity: that can
      // only come from a sibling tab. Signing in here starts from `null`, and
      // signing out here navigates to /login, so neither reaches this branch.
      if (previous !== null) {
        if (user) toast({ title: t("sessionSwitched", { email: user.email }), variant: "warning" });
        else toast({ title: t("sessionEnded"), variant: "warning" });
      }
    }
    lastUserId.current = id;
  }, [user, isPending, qc, t]);

  useEffect(() => {
    setPending(isPending);
    if (!isPending && user) {
      setUser(user);
    } else if (!isPending && !user) {
      clearUser();
    }
  }, [user, isPending, setUser, clearUser, setPending]);
}

export function useLogin() {
  const qc = useQueryClient();
  const { setUser } = useAuthStore();
  return useMutation({
    mutationFn: async (body: CredentialsInput) => asUser(await platform.auth.login(body.email, body.password)),
    onSuccess: (user) => {
      resetPlatformClientState(user.id);
      qc.clear();
      setUser(user);
      qc.setQueryData(authKeys.me, user);
    },
  });
}

export function useRegister() {
  const qc = useQueryClient();
  const { setUser } = useAuthStore();
  return useMutation({
    mutationFn: async (body: CredentialsInput) => asUser(await platform.auth.register(body.email, body.password)),
    onSuccess: (user) => {
      resetPlatformClientState(user.id);
      qc.clear();
      setUser(user);
      qc.setQueryData(authKeys.me, user);
    },
  });
}

export function useLogout() {
  const qc = useQueryClient();
  const { clearUser } = useAuthStore();
  return useMutation({
    mutationFn: () => platform.auth.logout(),
    onSuccess: () => {
      clearUser();
      resetPlatformClientState();
      qc.clear();
    },
  });
}

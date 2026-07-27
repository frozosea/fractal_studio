"use client";

import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { useAuthStore } from "@/stores/auth-store";
import type { CredentialsInput, UserView } from "@/types/auth";
import { useEffect } from "react";
import { platform, PlatformApiError, type PlatformUser } from "@/lib/api/platform";

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
    staleTime: 10 * 60_000,
    gcTime: 30 * 60_000,
  });
}

export function useSyncAuth() {
  const { data: user, isPending } = useCurrentUser();
  const { setUser, clearUser, setPending } = useAuthStore();

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
      qc.clear();
    },
  });
}

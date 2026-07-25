"use client";

import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { useAuthStore } from "@/stores/auth-store";
import type { CredentialsInput, UserView } from "@/types/auth";
import { useEffect } from "react";

export const authKeys = {
  me: ["me"] as const,
};

// Simulate API calls — replace with real backend when available
const simulateLogin = async (
  credentials: CredentialsInput
): Promise<UserView> => {
  await new Promise((r) => setTimeout(r, 800));
  // In production, POST to /v1/auth/login
  return {
    id: crypto.randomUUID(),
    email: credentials.email,
    displayName: credentials.email.split("@")[0] ?? "User",
  };
};

const simulateRegister = async (
  credentials: CredentialsInput
): Promise<UserView> => {
  await new Promise((r) => setTimeout(r, 800));
  // In production, POST to /v1/auth/register
  return {
    id: crypto.randomUUID(),
    email: credentials.email,
    displayName: credentials.email.split("@")[0] ?? "User",
  };
};

const simulateMe = async (): Promise<UserView | null> => {
  // Check localStorage for persisted session
  const stored = localStorage.getItem("fs_user");
  if (stored) {
    try {
      return JSON.parse(stored) as UserView;
    } catch {
      return null;
    }
  }
  return null;
};

// Sync hook: on mount, load from localStorage
export function useCurrentUser() {
  return useQuery({
    queryKey: authKeys.me,
    queryFn: async () => {
      const user = await simulateMe();
      return user;
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
    mutationFn: (body: CredentialsInput) => simulateLogin(body),
    onSuccess: (user) => {
      localStorage.setItem("fs_user", JSON.stringify(user));
      setUser(user);
      qc.invalidateQueries({ queryKey: authKeys.me });
    },
  });
}

export function useRegister() {
  const qc = useQueryClient();
  const { setUser } = useAuthStore();
  return useMutation({
    mutationFn: (body: CredentialsInput) => simulateRegister(body),
    onSuccess: (user) => {
      localStorage.setItem("fs_user", JSON.stringify(user));
      setUser(user);
      qc.invalidateQueries({ queryKey: authKeys.me });
    },
  });
}

export function useLogout() {
  const qc = useQueryClient();
  const { clearUser } = useAuthStore();
  return useMutation({
    mutationFn: async () => {
      localStorage.removeItem("fs_user");
    },
    onSuccess: () => {
      clearUser();
      qc.clear();
    },
  });
}

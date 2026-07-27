"use client";

import { createContext, useContext } from "react";
import { useRouter, usePathname } from "@/i18n/navigation";
import { useQueryClient } from "@tanstack/react-query";
import { useAuthStore } from "@/stores/auth-store";
import { useSyncAuth, useLogin as useLoginMutation, useRegister as useRegisterMutation, useLogout as useLogoutMutation } from "@/lib/hooks/use-auth";
import type { CredentialsInput, UserView } from "@/types/auth";
import { useEffect } from "react";

interface AuthContextValue {
  user: UserView | null;
  isAuthenticated: boolean;
  isPending: boolean;
  login: (credentials: CredentialsInput) => Promise<void>;
  register: (credentials: CredentialsInput) => Promise<void>;
  logout: () => Promise<void>;
}

const AuthContext = createContext<AuthContextValue | null>(null);

export function useAuth(): AuthContextValue {
  const ctx = useContext(AuthContext);
  if (!ctx) throw new Error("useAuth must be used within <AuthProvider>");
  return ctx;
}

export function AuthProvider({ children }: { children: React.ReactNode }) {
  const router = useRouter();
  const pathname = usePathname();
  const queryClient = useQueryClient();
  const { currentUser, isAuthenticated, isPending } = useAuthStore();

  useSyncAuth();

  const loginMutation = useLoginMutation();
  const registerMutation = useRegisterMutation();
  const logoutMutation = useLogoutMutation();

  // Redirect to login for protected routes
  const isAuthPage = pathname === "/login" || pathname === "/register";
  useEffect(() => {
    if (!isPending && !isAuthenticated && !isAuthPage) {
      router.push("/login");
    }
    if (!isPending && isAuthenticated && isAuthPage) {
      router.push("/studio");
    }
  }, [isAuthenticated, isPending, isAuthPage, router]);

  const login = async (credentials: CredentialsInput) => {
    await loginMutation.mutateAsync(credentials);
    router.push("/studio");
  };

  const register = async (credentials: CredentialsInput) => {
    await registerMutation.mutateAsync(credentials);
    router.push("/studio");
  };

  const logout = async () => {
    try {
      await logoutMutation.mutateAsync();
    } finally {
      queryClient.clear();
      router.push("/login");
    }
  };

  return (
    <AuthContext.Provider
      value={{
        user: currentUser,
        isAuthenticated,
        isPending,
        login,
        register,
        logout,
      }}
    >
      {children}
    </AuthContext.Provider>
  );
}

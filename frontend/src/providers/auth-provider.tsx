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

const INTENDED_PATH_KEY = "fractal-studio:intended-path";
const DEFAULT_LANDING = "/studio";

/**
 * Pages an anonymous visitor is allowed to see. The marketing surface has to be
 * readable before signing up, so these are the only routes that do not bounce
 * to /login. Everything else stays gated.
 */
const PUBLIC_PATHS = new Set(["/", "/tutorial", "/help", "/login", "/register"]);
const PUBLIC_PATH_PREFIXES = ["/creator/"];

export function isPublicPath(pathname: string): boolean {
  if (PUBLIC_PATHS.has(pathname)) return true;
  return PUBLIC_PATH_PREFIXES.some((prefix) => pathname.startsWith(prefix));
}

/**
 * Where to land after signing in. Kept in sessionStorage rather than a query
 * parameter so it is scoped to this tab: two tabs bounced to /login at the
 * same time each return to their own page.
 */
function rememberIntendedPath(pathname: string): void {
  // A public page is never worth returning to: someone who signs up from the
  // landing page wants the workbench, not the brochure.
  if (isPublicPath(pathname)) return;
  try {
    window.sessionStorage.setItem(INTENDED_PATH_KEY, pathname);
  } catch {
    /* private mode with storage disabled — fall back to the default landing */
  }
}

function takeIntendedPath(): string {
  try {
    const stored = window.sessionStorage.getItem(INTENDED_PATH_KEY);
    window.sessionStorage.removeItem(INTENDED_PATH_KEY);
    // Only ever a same-origin app path, never an absolute or protocol-relative
    // URL an attacker could have planted to bounce the user off-site.
    if (stored && stored.startsWith("/") && !stored.startsWith("//")) return stored;
  } catch {
    /* ignore */
  }
  return DEFAULT_LANDING;
}

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

  // Redirect to login for protected routes. Signed-in visitors are only moved
  // off the two auth forms — they may legitimately be reading /help or a
  // creator page, so the rest of the public surface never force-redirects.
  const isAuthPage = pathname === "/login" || pathname === "/register";
  const isPublic = isPublicPath(pathname);
  useEffect(() => {
    if (!isPending && !isAuthenticated && !isPublic) {
      rememberIntendedPath(pathname);
      router.push("/login");
    }
    if (!isPending && isAuthenticated && isAuthPage) {
      router.push(takeIntendedPath());
    }
  }, [isAuthenticated, isPending, isAuthPage, isPublic, pathname, router]);

  const login = async (credentials: CredentialsInput) => {
    await loginMutation.mutateAsync(credentials);
    router.push(takeIntendedPath());
  };

  const register = async (credentials: CredentialsInput) => {
    await registerMutation.mutateAsync(credentials);
    router.push(takeIntendedPath());
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

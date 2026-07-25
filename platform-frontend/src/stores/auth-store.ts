"use client";

import { create } from "zustand";
import type { UserView } from "@/types/auth";

interface AuthState {
  currentUser: UserView | null;
  isAuthenticated: boolean;
  isPending: boolean;

  setUser: (user: UserView) => void;
  clearUser: () => void;
  setPending: (pending: boolean) => void;
}

export const useAuthStore = create<AuthState>((set) => ({
  currentUser: null,
  isAuthenticated: false,
  isPending: true,

  setUser: (user) =>
    set({ currentUser: user, isAuthenticated: true, isPending: false }),
  clearUser: () =>
    set({ currentUser: null, isAuthenticated: false, isPending: false }),
  setPending: (pending) => set({ isPending: pending }),
}));

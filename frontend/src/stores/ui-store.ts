import { create } from 'zustand';

export interface UiState {
  sidebarOpen: boolean;
  sidebarTab: string;
  toggleSidebar: () => void;
  setSidebarOpen: (open: boolean) => void;
  setSidebarTab: (tab: string) => void;
}

export const useUiStore = create<UiState>()((set) => ({
  sidebarOpen: true,
  sidebarTab: 'controls',

  toggleSidebar: () => set((state) => ({ sidebarOpen: !state.sidebarOpen })),
  setSidebarOpen: (open: boolean) => set({ sidebarOpen: open }),
  setSidebarTab: (tab: string) => set({ sidebarTab: tab }),
}));

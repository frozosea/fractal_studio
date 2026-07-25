"use client";
import * as React from "react";
import { ToastProvider, ToastViewport, Toast, ToastTitle, ToastDescription, ToastClose, ToastAction } from "@/components/ui/toast";

interface ToasterToast {
  id: string;
  title?: string;
  description?: string;
  action?: {
    label: string;
    onClick: () => void;
  };
  variant?: "default" | "destructive" | "success" | "warning";
}

interface ToastState {
  toasts: ToasterToast[];
}

// Simple toast state management
let listeners: Array<(state: ToastState) => void> = [];
let memoryState: ToastState = { toasts: [] };
let toastCount = 0;

function dispatch(toast: Omit<ToasterToast, "id">) {
  const id = String(++toastCount);
  memoryState = {
    ...memoryState,
    toasts: [...memoryState.toasts, { ...toast, id }],
  };
  listeners.forEach((listener) => listener(memoryState));

  // Auto-dismiss after 5 seconds
  setTimeout(() => {
    memoryState = {
      ...memoryState,
      toasts: memoryState.toasts.filter((t) => t.id !== id),
    };
    listeners.forEach((listener) => listener(memoryState));
  }, 5000);

  return id;
}

function useToast() {
  const [state, setState] = React.useState<ToastState>(memoryState);

  React.useEffect(() => {
    listeners.push(setState);
    return () => {
      listeners = listeners.filter((l) => l !== setState);
    };
  }, []);

  return {
    ...state,
    toast: (props: Omit<ToasterToast, "id">) => dispatch(props),
    dismiss: (toastId?: string) => {
      memoryState = {
        ...memoryState,
        toasts: toastId
          ? memoryState.toasts.filter((t) => t.id !== toastId)
          : [],
      };
      listeners.forEach((listener) => listener(memoryState));
    },
  };
}

function Toaster() {
  const { toasts } = useToast();

  return (
    <ToastProvider>
      {toasts.map(({ id, title, description, action, variant }) => (
        <Toast key={id} variant={variant}>
          <div className="grid gap-1">
            {title && <ToastTitle>{title}</ToastTitle>}
            {description && <ToastDescription>{description}</ToastDescription>}
          </div>
          {action && (
            <ToastAction altText={action.label} onClick={action.onClick}>
              {action.label}
            </ToastAction>
          )}
          <ToastClose />
        </Toast>
      ))}
      <ToastViewport />
    </ToastProvider>
  );
}

export { Toaster, useToast, dispatch as toast };

"use client";

import { useState } from "react";
import { useForm } from "react-hook-form";
import { zodResolver } from "@hookform/resolvers/zod";
import { z } from "zod";
import { useAuth } from "@/providers/auth-provider";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Link } from "@/i18n/navigation";
import { PlatformApiError } from "@/lib/api/platform";
import { UserPlus, Mail, Lock } from "lucide-react";

const registerSchema = z
  .object({
    email: z.string().email("Invalid email"),
  password: z.string().min(12, "Password must be at least 12 characters"),
    confirmPassword: z.string(),
  })
  .refine((data) => data.password === data.confirmPassword, {
    message: "Passwords do not match",
    path: ["confirmPassword"],
  });

type RegisterFormData = z.infer<typeof registerSchema>;

export function RegisterForm() {
  const { register: registerUser } = useAuth();
  const [serverError, setServerError] = useState<string | null>(null);

  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<RegisterFormData>({
    resolver: zodResolver(registerSchema),
  });

  const onSubmit = async (data: RegisterFormData) => {
    try {
      setServerError(null);
      await registerUser({ email: data.email, password: data.password });
    } catch (error) {
      setServerError(
        error instanceof PlatformApiError && error.code === "email_already_registered"
          ? "This email already has an account. Sign in instead."
          : "Registration failed. Please try again.",
      );
    }
  };

  return (
    <div className="w-full max-w-md">
      {/* Gradient header */}
      <div className="mb-8 text-center">
        <div
          className="mx-auto mb-4 flex h-14 w-14 items-center justify-center rounded-2xl"
          style={{
            background:
              "linear-gradient(135deg, hsl(178 75% 45%) 0%, hsl(271 85% 40%) 100%)",
            boxShadow:
              "0 0 30px hsl(178 75% 50% / 0.2), 0 0 60px hsl(271 85% 50% / 0.06)",
          }}
        >
          <UserPlus className="h-6 w-6 text-white" />
        </div>
        <h1 className="gradient-text text-2xl font-semibold tracking-wide">
          Fractal Studio
        </h1>
        <p
          className="mt-2 text-sm tracking-wide"
          style={{ color: "hsl(220 16% 45%)" }}
        >
          Berkeley · Fractal Relaxation
        </p>
      </div>

      {/* Glass card with cyan tint */}
      <div className="glass-panel-cyan p-8">
        <h2 className="mb-6 text-center text-sm font-medium tracking-wide text-white/60">
          Create account
        </h2>

        <form onSubmit={handleSubmit(onSubmit)} className="space-y-4">
          <div className="space-y-2">
            <div className="relative">
              <Mail className="absolute left-3 top-2.5 h-4 w-4 text-white/20" />
              <Input
                {...register("email")}
                type="email"
                placeholder="Email"
                className="pl-10"
                autoComplete="email"
              />
            </div>
            {errors.email && (
              <p className="text-xs text-red-400/70">{errors.email.message}</p>
            )}
          </div>

          <div className="space-y-2">
            <div className="relative">
              <Lock className="absolute left-3 top-2.5 h-4 w-4 text-white/20" />
              <Input
                {...register("password")}
                type="password"
                placeholder="Password"
                className="pl-10"
                autoComplete="new-password"
              />
            </div>
            {errors.password && (
              <p className="text-xs text-red-400/70">
                {errors.password.message}
              </p>
            )}
          </div>

          <div className="space-y-2">
            <div className="relative">
              <Lock className="absolute left-3 top-2.5 h-4 w-4 text-white/20" />
              <Input
                {...register("confirmPassword")}
                type="password"
                placeholder="Confirm password"
                className="pl-10"
                autoComplete="new-password"
              />
            </div>
            {errors.confirmPassword && (
              <p className="text-xs text-red-400/70">
                {errors.confirmPassword.message}
              </p>
            )}
          </div>

          {serverError && (
            <p className="text-center text-sm text-red-400/70">
              {serverError}
            </p>
          )}

          <Button
            type="submit"
            variant="fractal"
            className="w-full btn-glow"
            disabled={isSubmitting}
            loading={isSubmitting}
          >
            Create account
          </Button>

          <p className="text-center text-sm text-white/25">
            Already have an account?{" "}
            <Link
              href="/login"
              className="text-primary/70 hover:text-primary transition-colors"
            >
              Sign in
            </Link>
          </p>
        </form>
      </div>
    </div>
  );
}

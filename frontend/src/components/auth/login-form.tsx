"use client";

import { useMemo, useState } from "react";
import { useForm } from "react-hook-form";
import { zodResolver } from "@hookform/resolvers/zod";
import { z } from "zod";
import { ArrowRight, LockKeyhole, Mail } from "lucide-react";
import { useTranslations } from "next-intl";
import { useAuth } from "@/providers/auth-provider";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Link } from "@/i18n/navigation";

type LoginFormData = { email: string; password: string };

export function LoginForm() {
  const t = useTranslations("auth");
  const { login } = useAuth();
  const [serverError, setServerError] = useState<string | null>(null);
  const schema = useMemo(() => z.object({
    email: z.string().email(t("errors.invalidEmail")),
    password: z.string().min(12, t("errors.passwordLength")),
  }), [t]);
  const { register, handleSubmit, formState: { errors, isSubmitting } } = useForm<LoginFormData>({
    resolver: zodResolver(schema),
  });

  const onSubmit = async (data: LoginFormData) => {
    try {
      setServerError(null);
      await login(data);
    } catch {
      setServerError(t("errors.loginFailed"));
    }
  };

  return (
    <div className="w-full max-w-[30rem]">
      <header className="mb-5 border-b border-hairline/10 pb-5">
        <div className="mb-5 flex items-center justify-between">
          <span className="instrument-kicker">AUTH / 01</span>
          <span className="font-mono text-[10px] uppercase tracking-[0.14em] text-ink/25">z ↦ z² + c</span>
        </div>
        <h1 className="text-2xl font-medium tracking-tight text-ink">{t("signInTitle")}</h1>
        <p className="mt-2 text-sm leading-6 text-ink/60">{t("signInSubtitle")}</p>
      </header>

      <section className="auth-panel p-5 sm:p-7">
        <div className="mb-6 flex items-center gap-3 border-b border-hairline/[0.07] pb-4">
          <span className="grid h-9 w-9 place-items-center border border-amber-300/40 bg-amber-400/[0.07]"><LockKeyhole className="h-4 w-4 text-amber-200/75" /></span>
          <div><p className="text-sm text-ink/80">{t("accountAccess")}</p><p className="mt-0.5 font-mono text-[10px] uppercase tracking-wider text-ink/30">{t("secureSession")}</p></div>
        </div>

        <form onSubmit={handleSubmit(onSubmit)} className="space-y-4">
          <AuthField error={errors.email?.message} icon={<Mail className="h-3.5 w-3.5" />} label={t("emailLabel")}>
            <Input {...register("email")} type="email" placeholder={t("emailPlaceholder")} className="auth-control h-10 rounded-none pl-10 font-mono text-sm" autoComplete="email" />
          </AuthField>
          <AuthField error={errors.password?.message} icon={<LockKeyhole className="h-3.5 w-3.5" />} label={t("passwordLabel")}>
            <Input {...register("password")} type="password" placeholder={t("passwordPlaceholder")} className="auth-control h-10 rounded-none pl-10 font-mono text-sm" autoComplete="current-password" />
          </AuthField>

          {serverError && <p className="border-l-2 border-red-400/60 bg-red-400/[0.06] px-3 py-2 text-sm text-red-200/80">{serverError}</p>}

          <Button type="submit" className="h-10 w-full rounded-none border border-amber-300/35 bg-amber-500/15 text-amber-100 hover:bg-amber-500/25" disabled={isSubmitting} loading={isSubmitting}>
            {t("signIn")}<ArrowRight className="h-4 w-4" />
          </Button>
          <p className="text-center text-sm text-ink/60">{t("noAccount")} <Link href="/register" className="text-amber-200/75 transition-colors hover:text-amber-100">{t("createOne")}</Link></p>
        </form>
      </section>
    </div>
  );
}

function AuthField({ label, icon, error, children }: { label: string; icon: React.ReactNode; error?: string; children: React.ReactNode }) {
  return (
    <label className="block">
      <span className="mb-1.5 block font-mono text-[10px] uppercase tracking-[0.14em] text-ink/60">{label}</span>
      <span className="relative block"><span className="pointer-events-none absolute left-3 top-3 text-ink/25">{icon}</span>{children}</span>
      {error && <span className="mt-1.5 block text-xs text-red-300/75">{error}</span>}
    </label>
  );
}

"use client";

import { ChangeEvent, useEffect, useMemo, useState } from "react";
import { useQueryClient } from "@tanstack/react-query";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { toast } from "@/components/ui/toaster";
import { useAuth } from "@/providers/auth-provider";
import { authKeys } from "@/lib/hooks/use-auth";
import { platform, PlatformApiError, type CreatorBalance, type PayoutRequest } from "@/lib/api/platform";

const statusLabels: Record<PayoutRequest["status"], string> = {
  pending: "Pending review", paid: "Paid", rejected: "Rejected", cancelled: "Cancelled",
};
const statusClass: Record<PayoutRequest["status"], string> = {
  pending: "border-amber-400/25 bg-amber-400/10 text-amber-200",
  paid: "border-emerald-400/25 bg-emerald-400/10 text-emerald-200",
  rejected: "border-red-400/25 bg-red-400/10 text-red-200",
  cancelled: "border-white/10 bg-white/[0.04] text-muted-foreground",
};

function text(error: unknown): string {
  return error instanceof Error ? error.message : "Request failed";
}

export default function PayoutsPage() {
  const queryClient = useQueryClient();
  const { user } = useAuth();
  const [rows, setRows] = useState<PayoutRequest[]>([]);
  const [balance, setBalance] = useState<CreatorBalance | null>(null);
  const [amount, setAmount] = useState("10.00");
  const [file, setFile] = useState<File | null>(null);
  const [handle, setHandle] = useState("");
  const [displayName, setDisplayName] = useState("");
  const [savingProfile, setSavingProfile] = useState(false);
  const [requestingPayout, setRequestingPayout] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [statusFilter, setStatusFilter] = useState<"all" | PayoutRequest["status"]>("all");
  const [minAmount, setMinAmount] = useState("");
  const [maxAmount, setMaxAmount] = useState("");
  const [dateFrom, setDateFrom] = useState("");
  const [dateTo, setDateTo] = useState("");

  const refresh = () =>
    void Promise.all([platform.payouts.list(), platform.payouts.balance()])
      .then(([requests, creatorBalance]) => { setRows(requests.data); setBalance(creatorBalance); })
      .catch((reason: unknown) => setError(text(reason)));

  const isCreator = user?.roles.includes("creator") ?? false;
  useEffect(() => { if (isCreator) refresh(); }, [isCreator]);
  const pendingRequest = rows.find((row) => row.status === "pending");
  const availableBalance = Number(balance?.availableAmount ?? 0);
  const requestedAmount = Number(amount);
  const insufficientBalance = !Number.isFinite(requestedAmount) || requestedAmount <= 0 || requestedAmount > availableBalance;
  const filteredRows = useMemo(() => rows.filter((row) => {
    const value = Number(row.amount); const created = row.createdAt.slice(0, 10);
    return (statusFilter === "all" || row.status === statusFilter)
      && (!minAmount || value >= Number(minAmount))
      && (!maxAmount || value <= Number(maxAmount))
      && (!dateFrom || created >= dateFrom)
      && (!dateTo || created <= dateTo);
  }), [rows, statusFilter, minAmount, maxAmount, dateFrom, dateTo]);
  const resetFilters = () => { setStatusFilter("all"); setMinAmount(""); setMaxAmount(""); setDateFrom(""); setDateTo(""); };

  const saveCreatorProfile = async () => {
    const normalizedHandle = handle.trim();
    const normalizedDisplayName = displayName.trim();
    if (!/^[a-z0-9_]{3,32}$/.test(normalizedHandle)) {
      setError("Handle: 3–32 lowercase letters, numbers, or underscores.");
      return;
    }
    if (!normalizedDisplayName) {
      setError("Enter a display name.");
      return;
    }
    setSavingProfile(true);
    setError(null);
    try {
      const user = await platform.auth.creatorProfile(normalizedHandle, normalizedDisplayName);
      queryClient.setQueryData(authKeys.me, user);
      toast({
        title: "Creator profile created",
        description: `You can now publish listings as @${user.creatorProfile?.handle ?? normalizedHandle}.`,
        variant: "success",
      });
      refresh();
    } catch (reason) {
      setError(text(reason));
    } finally {
      setSavingProfile(false);
    }
  };

  const request = async () => {
    if (pendingRequest) {
      setError("You already have a payout request pending. Cancel it before creating another one.");
      return;
    }
    if (!file) {
      setError("Choose payout QR code");
      return;
    }
    setRequestingPayout(true);
    setError(null);
    try {
      await platform.payouts.create(amount, file);
      refresh();
    } catch (reason) {
      if (reason instanceof PlatformApiError && reason.code === "payout_request_pending") {
        setError("You already have a payout request pending. Cancel it before creating another one.");
      } else if (reason instanceof PlatformApiError && reason.code === "insufficient_creator_balance") {
        setError("Payout amount exceeds your available creator balance.");
      } else {
        setError(text(reason));
      }
      refresh();
    } finally {
      setRequestingPayout(false);
    }
  };

  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-2xl font-semibold">Creator payouts</h1>
        <p className="text-muted-foreground">Upload Alipay receiving QR. Finance operator sends manual transfer.</p>
      </div>
      {!isCreator && <section className="max-w-lg space-y-3 rounded-xl border border-white/10 p-4">
        <h2 className="font-medium">Become creator</h2>
        <Input
          value={handle}
          placeholder="handle (lowercase, e.g. fractal_artist)"
          maxLength={32}
          onChange={(event) => setHandle(event.target.value.toLowerCase())}
        />
        <Input value={displayName} placeholder="display name" maxLength={120} onChange={(event) => setDisplayName(event.target.value)} />
        <Button variant="outline" loading={savingProfile} disabled={!handle || !displayName} onClick={() => void saveCreatorProfile()}>
          Save creator profile
        </Button>
      </section>}
      {isCreator && <div className="grid items-start gap-5 xl:grid-cols-[minmax(20rem,0.8fr)_minmax(0,1.2fr)]">
        <section className="space-y-3 rounded-xl border border-white/10 p-4">
          <h2 className="font-medium">Request a payout</h2>
          <div className="rounded-lg border border-white/10 bg-white/[0.03] p-3 text-sm"><span className="text-muted-foreground">Available to withdraw</span><p className="mt-1 text-lg font-medium">{balance ? `${balance.availableAmount} ${balance.currency}` : "Loading balance…"}</p>{balance && availableBalance <= 0 && <p className="mt-1 text-xs text-muted-foreground">Funds appear here after a buyer pays for one of your listings.</p>}</div>
          <Input value={amount} onChange={(event) => setAmount(event.target.value)} placeholder="Amount CNY" disabled={!balance || availableBalance <= 0} />
          <input type="file" accept="image/png,image/jpeg" onChange={(event: ChangeEvent<HTMLInputElement>) => setFile(event.target.files?.[0] ?? null)} />
          {pendingRequest && <p className="rounded-lg border border-amber-400/25 bg-amber-400/10 p-3 text-sm text-amber-200">A payout request is already pending. Cancel it in the history if you need to submit a new one.</p>}
          {balance && insufficientBalance && !pendingRequest && <p className="text-sm text-amber-200">Enter an amount no greater than your available balance.</p>}
          <Button loading={requestingPayout} disabled={!file || Boolean(pendingRequest) || !balance || insufficientBalance} onClick={() => void request()}>{pendingRequest ? "Payout pending" : "Request payout"}</Button>
        </section>
        <section className="rounded-xl border border-white/10 p-4">
          <div className="flex flex-wrap items-center justify-between gap-2"><div><h2 className="font-medium">Payout history</h2><p className="text-sm text-muted-foreground">{filteredRows.length} of {rows.length} transactions</p></div><Button size="sm" variant="outline" onClick={resetFilters}>Clear filters</Button></div>
          <div className="mt-4 grid gap-2 sm:grid-cols-2 lg:grid-cols-3"><label className="text-xs text-muted-foreground">Status<select className="mt-1 h-10 w-full rounded-lg border border-deep-slate bg-deep-slate/50 px-3 text-sm text-foreground" value={statusFilter} onChange={(event) => setStatusFilter(event.target.value as typeof statusFilter)}><option value="all">All statuses</option>{Object.entries(statusLabels).map(([value, label]) => <option key={value} value={value}>{label}</option>)}</select></label><label className="text-xs text-muted-foreground">Minimum CNY<Input className="mt-1" type="number" min="0" value={minAmount} onChange={(event) => setMinAmount(event.target.value)} /></label><label className="text-xs text-muted-foreground">Maximum CNY<Input className="mt-1" type="number" min="0" value={maxAmount} onChange={(event) => setMaxAmount(event.target.value)} /></label><label className="text-xs text-muted-foreground">From<Input className="mt-1" type="date" value={dateFrom} onChange={(event) => setDateFrom(event.target.value)} /></label><label className="text-xs text-muted-foreground">To<Input className="mt-1" type="date" value={dateTo} onChange={(event) => setDateTo(event.target.value)} /></label></div>
          <div className="mt-4 space-y-2">{filteredRows.length ? filteredRows.map((row) => <article key={row.id} className="rounded-lg border border-white/10 bg-white/[0.02] p-3"><div className="flex flex-wrap items-start justify-between gap-3"><div><p className="font-medium">{row.amount} {row.currency}</p><p className="mt-1 text-xs text-muted-foreground">Requested {new Date(row.createdAt).toLocaleString()}</p>{row.paidAt && <p className="mt-1 text-xs text-emerald-300">Paid {new Date(row.paidAt).toLocaleString()}</p>}{row.rejectionReason && <p className="mt-1 text-xs text-red-300">Reason: {row.rejectionReason}</p>}</div><div className="flex items-center gap-2"><span className={`rounded-full border px-2 py-1 text-xs ${statusClass[row.status]}`}>{statusLabels[row.status]}</span>{row.status === "pending" && <Button size="sm" variant="outline" onClick={() => void platform.payouts.cancel(row.id).then(refresh).catch((reason: unknown) => setError(text(reason)))}>Cancel</Button>}</div></div></article>) : <p className="rounded-lg border border-dashed border-white/10 p-6 text-center text-sm text-muted-foreground">No payouts match these filters.</p>}</div>
        </section>
      </div>}
      {error && <p className="text-red-400">{error}</p>}
    </div>
  );
}

"use client";

import { useEffect, useState } from "react";
import { useLocale, useTranslations } from "next-intl";
import { BarChart3, Boxes, RefreshCw, ShoppingBag, Users } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { useRouter } from "@/i18n/navigation";
import {
  platform,
  PlatformApiError,
  type AdminListing,
  type AdminStatistics,
  type AdminUser,
} from "@/lib/api/platform";
import { useAuth } from "@/providers/auth-provider";

type PrivilegedRole = "admin" | "finance_operator";
type UserStatusFilter = "all" | AdminUser["status"];
type UserRoleFilter = "all" | "admin" | "creator" | "finance_operator";
type ListingStatusFilter = "all" | AdminListing["status"];

const userStatuses: UserStatusFilter[] = ["all", "active", "disabled"];
const userRoles: UserRoleFilter[] = ["all", "admin", "creator", "finance_operator"];
const listingStatuses: ListingStatusFilter[] = ["all", "published", "draft", "unpublished", "archived"];

function privilegedRoles(user: AdminUser): PrivilegedRole[] {
  return user.roles.filter((role): role is PrivilegedRole => role === "admin" || role === "finance_operator");
}

export default function AdminPage() {
  const t = useTranslations("commerce.admin");
  const locale = useLocale();
  const router = useRouter();
  const { user, isPending } = useAuth();
  const allowed = Boolean(user?.roles.includes("admin"));
  const [statistics, setStatistics] = useState<AdminStatistics | null>(null);
  const [users, setUsers] = useState<AdminUser[]>([]);
  const [listings, setListings] = useState<AdminListing[]>([]);
  const [nextUserCursor, setNextUserCursor] = useState<string | null>(null);
  const [nextListingCursor, setNextListingCursor] = useState<string | null>(null);
  const [userQuery, setUserQuery] = useState("");
  const [userStatus, setUserStatus] = useState<UserStatusFilter>("all");
  const [userRole, setUserRole] = useState<UserRoleFilter>("all");
  const [listingQuery, setListingQuery] = useState("");
  const [listingStatus, setListingStatus] = useState<ListingStatusFilter>("all");
  const [loading, setLoading] = useState(true);
  const [busy, setBusy] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const errorText = (reason: unknown) => reason instanceof PlatformApiError
    ? t("requestWithCode", { code: reason.code })
    : t("requestFailed");
  const formatDate = (value: string) => new Intl.DateTimeFormat(locale, {
    dateStyle: "medium",
    timeStyle: "short",
  }).format(new Date(value));

  const loadStatistics = async () => setStatistics(await platform.admin.statistics());
  const loadUsers = async (append = false) => {
    const value = await platform.admin.users(
      { query: userQuery.trim(), status: userStatus, role: userRole },
      append ? nextUserCursor : null,
    );
    setUsers((current) => append ? [...current, ...value.data] : value.data);
    setNextUserCursor(value.page.nextCursor);
  };
  const loadListings = async (append = false) => {
    const value = await platform.admin.listings(
      { query: listingQuery.trim(), status: listingStatus },
      append ? nextListingCursor : null,
    );
    setListings((current) => append ? [...current, ...value.data] : value.data);
    setNextListingCursor(value.page.nextCursor);
  };
  const loadAll = async () => {
    setLoading(true);
    setError(null);
    try {
      await Promise.all([loadStatistics(), loadUsers(), loadListings()]);
    } catch (reason) {
      setError(errorText(reason));
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    if (!isPending && !allowed) router.replace("/studio");
  }, [allowed, isPending, router]);
  useEffect(() => {
    if (allowed) void loadAll();
    // Filters are submitted explicitly; typing must not trigger requests.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [allowed]);

  const updateUser = async (row: AdminUser, body: Parameters<typeof platform.admin.updateUser>[1]) => {
    setBusy(`user:${row.id}`);
    setError(null);
    try {
      await platform.admin.updateUser(row.id, body);
      await Promise.all([loadUsers(), loadStatistics()]);
    } catch (reason) {
      setError(errorText(reason));
    } finally {
      setBusy(null);
    }
  };

  const toggleRole = (row: AdminUser, role: PrivilegedRole) => {
    const current = privilegedRoles(row);
    const hasRole = current.includes(role);
    if (!window.confirm(t(hasRole ? "confirmRevokeRole" : "confirmGrantRole", {
      role: t(`role.${role}`),
      email: row.email,
    }))) return;
    const next = hasRole ? current.filter((value) => value !== role) : [...current, role];
    void updateUser(row, { privilegedRoles: next });
  };

  const toggleStatus = (row: AdminUser) => {
    const next = row.status === "active" ? "disabled" : "active";
    if (next === "disabled" && !window.confirm(t("confirmDisable", { email: row.email }))) return;
    void updateUser(row, { status: next });
  };

  const toggleMembership = (row: AdminUser) => {
    if (!window.confirm(t(row.member ? "confirmRevokeMembership" : "confirmGrantMembership", { email: row.email }))) return;
    void updateUser(row, { member: !row.member });
  };

  const moderate = (listing: AdminListing, action: "unpublish" | "archive") => {
    const reason = window.prompt(t(action === "archive" ? "archiveReason" : "unpublishReason"));
    if (!reason?.trim()) return;
    if (action === "archive" && !window.confirm(t("confirmArchive", { title: listing.title }))) return;
    setBusy(`listing:${listing.id}`);
    setError(null);
    void platform.admin.moderateListing(listing.id, action, reason.trim())
      .then(() => Promise.all([loadListings(), loadStatistics()]))
      .catch((reason: unknown) => setError(errorText(reason)))
      .finally(() => setBusy(null));
  };

  if (isPending || !allowed) {
    return <div className="p-6 text-sm text-muted-foreground">{t("checkingAccess")}</div>;
  }

  const statCards = statistics ? [
    { label: t("stats.users"), value: statistics.users.total, hint: t("stats.usersHint", { active: statistics.users.active, newCount: statistics.users.newLast30Days }), icon: Users },
    { label: t("stats.published"), value: statistics.market.published, hint: t("stats.marketHint", { total: statistics.market.listings, favorites: statistics.market.favorites }), icon: ShoppingBag },
    { label: t("stats.revenue"), value: `¥${statistics.commerce.marketplaceGrossCny}`, hint: t("stats.revenueHint", { membership: statistics.commerce.membershipRevenueCny, orders: statistics.commerce.fulfilled }), icon: BarChart3 },
    { label: t("stats.renders"), value: statistics.compute.renderJobs, hint: t("stats.rendersHint", { active: statistics.compute.active, failed: statistics.compute.failed }), icon: Boxes },
  ] : [];

  return (
    <div className="space-y-6">
      <div className="flex flex-wrap items-start justify-between gap-3">
        <div>
          <h1 className="text-2xl font-semibold">{t("title")}</h1>
          <p className="text-muted-foreground">{t("subtitle")}</p>
        </div>
        <Button variant="outline" loading={loading} onClick={() => void loadAll()}>
          <RefreshCw className="h-4 w-4" />{t("refresh")}
        </Button>
      </div>
      {error && <p className="rounded-lg border border-red-500/20 bg-red-500/10 p-3 text-sm text-red-300">{error}</p>}
      <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
        {statCards.map(({ label, value, hint, icon: Icon }) => (
          <article key={label} className="rounded-xl border border-white/10 bg-white/[0.025] p-4">
            <div className="flex items-center justify-between text-sm text-muted-foreground"><span>{label}</span><Icon className="h-4 w-4" /></div>
            <p className="mt-3 text-2xl font-semibold text-white/90">{value}</p>
            <p className="mt-1 text-xs text-muted-foreground">{hint}</p>
          </article>
        ))}
      </div>

      <Tabs defaultValue="accounts" className="space-y-4">
        <TabsList className="grid w-full max-w-md grid-cols-3">
          <TabsTrigger value="accounts">{t("tabs.accounts")}</TabsTrigger>
          <TabsTrigger value="market">{t("tabs.market")}</TabsTrigger>
          <TabsTrigger value="details">{t("tabs.details")}</TabsTrigger>
        </TabsList>

        <TabsContent value="accounts" className="space-y-4">
          <section className="grid gap-2 rounded-xl border border-white/10 p-4 md:grid-cols-[minmax(12rem,1fr)_12rem_12rem_auto]">
            <Input value={userQuery} placeholder={t("accounts.searchPlaceholder")} onChange={(event) => { setUserQuery(event.target.value); setNextUserCursor(null); }} onKeyDown={(event) => { if (event.key === "Enter") void loadUsers(); }} />
            <select className="h-10 rounded-lg border border-white/[0.08] bg-[#15181e] px-3 text-sm text-white/75" value={userStatus} onChange={(event) => { setUserStatus(event.target.value as UserStatusFilter); setNextUserCursor(null); }}>{userStatuses.map((value) => <option key={value} value={value}>{t(`userStatus.${value}`)}</option>)}</select>
            <select className="h-10 rounded-lg border border-white/[0.08] bg-[#15181e] px-3 text-sm text-white/75" value={userRole} onChange={(event) => { setUserRole(event.target.value as UserRoleFilter); setNextUserCursor(null); }}>{userRoles.map((value) => <option key={value} value={value}>{t(`role.${value}`)}</option>)}</select>
            <Button variant="outline" onClick={() => void loadUsers()}>{t("search")}</Button>
          </section>
          <div className="space-y-3">
            {users.map((row) => {
              const isSelf = row.id === user?.id;
              const rowBusy = busy === `user:${row.id}`;
              return <article key={row.id} className="rounded-xl border border-white/10 bg-white/[0.02] p-4">
                <div className="flex flex-wrap items-start justify-between gap-4">
                  <div className="min-w-0">
                    <p className="truncate font-medium text-white/90">{row.email}</p>
                    <p className="mt-1 text-xs text-muted-foreground">{row.creatorProfile ? `${row.creatorProfile.displayName} · @${row.creatorProfile.handle}` : t("accounts.noCreatorProfile")} · {formatDate(row.createdAt)}</p>
                    <div className="mt-2 flex flex-wrap gap-1.5">
                      <Badge variant={row.status === "active" ? "success" : "error"}>{t(`userStatus.${row.status}`)}</Badge>
                      {row.member && <Badge variant="warning">{t("accounts.member")}</Badge>}
                      {row.roles.map((role) => <Badge key={role} variant="outline">{t(`role.${role}`)}</Badge>)}
                    </div>
                    <p className="mt-2 text-xs text-muted-foreground">{t("accounts.counts", { assets: row.assetCount, listings: row.listingCount, orders: row.fulfilledOrderCount })}</p>
                  </div>
                  <div className="flex max-w-xl flex-wrap justify-end gap-2">
                    <Button size="sm" variant="outline" loading={rowBusy} disabled={isSelf} onClick={() => toggleStatus(row)}>{t(row.status === "active" ? "accounts.disable" : "accounts.enable")}</Button>
                    <Button size="sm" variant="outline" loading={rowBusy} onClick={() => toggleMembership(row)}>{t(row.member ? "accounts.revokeMembership" : "accounts.grantMembership")}</Button>
                    <Button size="sm" variant="outline" loading={rowBusy} onClick={() => toggleRole(row, "finance_operator")}>{t(row.roles.includes("finance_operator") ? "accounts.revokeFinance" : "accounts.grantFinance")}</Button>
                    <Button size="sm" variant="outline" loading={rowBusy} disabled={isSelf} onClick={() => toggleRole(row, "admin")}>{t(row.roles.includes("admin") ? "accounts.revokeAdmin" : "accounts.grantAdmin")}</Button>
                  </div>
                </div>
              </article>;
            })}
            {!loading && users.length === 0 && <p className="rounded-xl border border-dashed border-white/10 p-8 text-center text-sm text-muted-foreground">{t("accounts.empty")}</p>}
          </div>
          {nextUserCursor && <div className="flex justify-center"><Button variant="outline" onClick={() => void loadUsers(true)}>{t("loadMore")}</Button></div>}
        </TabsContent>

        <TabsContent value="market" className="space-y-4">
          <section className="grid gap-2 rounded-xl border border-white/10 p-4 md:grid-cols-[minmax(12rem,1fr)_14rem_auto]">
            <Input value={listingQuery} placeholder={t("market.searchPlaceholder")} onChange={(event) => { setListingQuery(event.target.value); setNextListingCursor(null); }} onKeyDown={(event) => { if (event.key === "Enter") void loadListings(); }} />
            <select className="h-10 rounded-lg border border-white/[0.08] bg-[#15181e] px-3 text-sm text-white/75" value={listingStatus} onChange={(event) => { setListingStatus(event.target.value as ListingStatusFilter); setNextListingCursor(null); }}>{listingStatuses.map((value) => <option key={value} value={value}>{t(`listingStatus.${value}`)}</option>)}</select>
            <Button variant="outline" onClick={() => void loadListings()}>{t("search")}</Button>
          </section>
          <div className="grid gap-4 xl:grid-cols-2">
            {listings.map((listing) => {
              const rowBusy = busy === `listing:${listing.id}`;
              return <article key={listing.id} className="overflow-hidden rounded-xl border border-white/10 bg-white/[0.02] sm:grid sm:grid-cols-[11rem_minmax(0,1fr)]">
                <div className="aspect-[4/3] bg-white/[0.04] sm:aspect-auto">
                  {listing.preview?.thumbnailUrl ? <img src={listing.preview.thumbnailUrl} alt={listing.title} className="h-full w-full object-cover" loading="lazy" /> : <div className="flex h-full min-h-32 items-center justify-center text-xs text-muted-foreground">{t("market.noPreview")}</div>}
                </div>
                <div className="min-w-0 p-4">
                  <div className="flex items-start justify-between gap-3"><div className="min-w-0"><h2 className="truncate font-medium text-white/90">{listing.title}</h2><p className="truncate text-xs text-muted-foreground">{listing.creatorDisplayName ?? listing.creatorHandle ?? listing.creatorEmail}</p></div><Badge variant={listing.status === "published" ? "success" : listing.status === "archived" ? "error" : "outline"}>{t(`listingStatus.${listing.status}`)}</Badge></div>
                  <p className="mt-3 line-clamp-2 text-sm text-muted-foreground">{listing.description || t("market.noDescription")}</p>
                  <p className="mt-2 text-xs text-muted-foreground">{listing.price} {listing.currency} · {t("market.activity", { favorites: listing.favoriteCount, sales: listing.saleCount })}</p>
                  <div className="mt-4 flex flex-wrap gap-2">
                    {listing.status === "published" && <Button size="sm" variant="outline" loading={rowBusy} onClick={() => moderate(listing, "unpublish")}>{t("market.unpublish")}</Button>}
                    {listing.status !== "archived" && <Button size="sm" variant="destructive" loading={rowBusy} onClick={() => moderate(listing, "archive")}>{t("market.archive")}</Button>}
                  </div>
                </div>
              </article>;
            })}
            {!loading && listings.length === 0 && <p className="rounded-xl border border-dashed border-white/10 p-8 text-center text-sm text-muted-foreground xl:col-span-2">{t("market.empty")}</p>}
          </div>
          {nextListingCursor && <div className="flex justify-center"><Button variant="outline" onClick={() => void loadListings(true)}>{t("loadMore")}</Button></div>}
        </TabsContent>

        <TabsContent value="details">
          {statistics && <div className="grid gap-4 lg:grid-cols-3">
            <section className="rounded-xl border border-white/10 p-4"><h2 className="font-medium">{t("details.accounts")}</h2><dl className="mt-3 space-y-2 text-sm">{[[t("details.active"), statistics.users.active], [t("details.disabled"), statistics.users.disabled], [t("details.members"), statistics.users.members], [t("details.creators"), statistics.users.creators], [t("details.admins"), statistics.users.admins]].map(([label, value]) => <div key={String(label)} className="flex justify-between gap-4"><dt className="text-muted-foreground">{label}</dt><dd>{value}</dd></div>)}</dl></section>
            <section className="rounded-xl border border-white/10 p-4"><h2 className="font-medium">{t("details.market")}</h2><dl className="mt-3 space-y-2 text-sm">{[[t("listingStatus.published"), statistics.market.published], [t("listingStatus.draft"), statistics.market.draft], [t("listingStatus.unpublished"), statistics.market.unpublished], [t("listingStatus.archived"), statistics.market.archived], [t("details.readyAssets"), statistics.market.readyAssets]].map(([label, value]) => <div key={String(label)} className="flex justify-between gap-4"><dt className="text-muted-foreground">{label}</dt><dd>{value}</dd></div>)}</dl></section>
            <section className="rounded-xl border border-white/10 p-4"><h2 className="font-medium">{t("details.commerce")}</h2><dl className="mt-3 space-y-2 text-sm">{[[t("details.orders"), statistics.commerce.orders], [t("details.pending"), statistics.commerce.pendingPayment], [t("details.exceptions"), statistics.commerce.paymentExceptions], [t("details.creatorRevenue"), `¥${statistics.commerce.creatorRevenueCny}`], [t("details.platformRevenue"), `¥${statistics.commerce.platformRevenueCny}`]].map(([label, value]) => <div key={String(label)} className="flex justify-between gap-4"><dt className="text-muted-foreground">{label}</dt><dd>{value}</dd></div>)}</dl></section>
          </div>}
        </TabsContent>
      </Tabs>
    </div>
  );
}

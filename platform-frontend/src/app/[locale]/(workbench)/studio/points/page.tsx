"use client";

import { useTranslations } from "next-intl";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { PointList } from "@/components/studio/point-list";
import { PointSearchPanel } from "@/components/studio/point-search-panel";
import { useSpecialPointsList } from "@/lib/hooks/use-points";
import { LoadingSpinner } from "@/components/shared/loading-spinner";

export default function PointsPage() {
  const t = useTranslations("points");
  const { data, isLoading } = useSpecialPointsList();

  return (
    <div className="space-y-6">
      <h1 className="text-2xl font-bold tracking-tight">{t("title")}</h1>
      <Tabs defaultValue="search">
        <TabsList>
          <TabsTrigger value="search">{t("search")}</TabsTrigger>
          <TabsTrigger value="library">{t("library")}</TabsTrigger>
        </TabsList>
        <TabsContent value="search" className="mt-4">
          <PointSearchPanel />
        </TabsContent>
        <TabsContent value="library" className="mt-4">
          {isLoading ? (
            <LoadingSpinner />
          ) : (
            <PointList points={data?.items ?? []} />
          )}
        </TabsContent>
      </Tabs>
    </div>
  );
}

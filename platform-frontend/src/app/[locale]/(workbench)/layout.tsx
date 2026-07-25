import { WorkbenchShell } from "@/components/layout/workbench-shell";

export default function WorkbenchLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return <WorkbenchShell>{children}</WorkbenchShell>;
}

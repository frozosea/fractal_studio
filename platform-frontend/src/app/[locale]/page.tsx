import { redirect } from "@/i18n/navigation";

export default function HomePage() {
  return redirect({ href: "/studio", locale: "zh" });
}

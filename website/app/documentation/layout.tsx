import { Breadcrumbs } from "@/components/shared/breadcrumbs";
import { DocsSidebar } from "@/components/docs/docs-sidebar";

export default function DocumentationLayout({ children }: { children: React.ReactNode }) {
  return (
    <div className="container py-10">
      <Breadcrumbs items={[{ title: "Documentation" }]} />
      <div className="mt-6 flex flex-col gap-10 lg:flex-row">
        <DocsSidebar />
        <div className="min-w-0 flex-1 pb-16">{children}</div>
      </div>
    </div>
  );
}

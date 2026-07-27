import Link from "next/link";
import { ArrowRight, Building2, ShieldCheck, Workflow } from "lucide-react";
import { Button } from "@/components/ui/button";
import { SectionHeading } from "@/components/shared/section-heading";

const items = [
  { icon: Building2, title: "Fleet management", description: "Every host and VM, one console." },
  { icon: ShieldCheck, title: "Compliance-ready", description: "SOC 2 and ISO 27001-aligned controls." },
  { icon: Workflow, title: "Policy automation", description: "Patch, quota, and retention policy at scale." },
];

export function EnterpriseTeaser() {
  return (
    <section className="container py-20">
      <div className="grid grid-cols-1 items-center gap-12 lg:grid-cols-2">
        <div>
          <SectionHeading
            eyebrow="Enterprise"
            title="Standardize virtualization across the entire organization"
            description="LocalHost Enterprise gives IT and platform teams centralized control over every host and VM, with role-based access, automated compliance, and disaster recovery built in."
          />
          <Button className="mt-8" asChild>
            <Link href="/enterprise">
              Explore Enterprise capabilities
              <ArrowRight className="h-4 w-4" />
            </Link>
          </Button>
        </div>
        <div className="grid grid-cols-1 gap-4 sm:grid-cols-3 lg:grid-cols-1">
          {items.map((item) => (
            <div key={item.title} className="flex items-start gap-4 rounded-lg border border-border p-5">
              <div className="flex h-10 w-10 shrink-0 items-center justify-center rounded-md bg-primary/10 text-primary">
                <item.icon className="h-5 w-5" />
              </div>
              <div>
                <h3 className="font-semibold">{item.title}</h3>
                <p className="mt-1 text-sm text-muted-foreground">{item.description}</p>
              </div>
            </div>
          ))}
        </div>
      </div>
    </section>
  );
}

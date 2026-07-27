import Link from "next/link";
import { ArrowRight } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Breadcrumbs } from "@/components/shared/breadcrumbs";
import { SectionHeading } from "@/components/shared/section-heading";
import { FeatureGrid } from "@/components/shared/feature-grid";
import { Timeline } from "@/components/shared/timeline";
import { StatBand } from "@/components/shared/stat-band";
import { enterpriseCapabilities, deploymentTimeline } from "@/lib/data/enterprise";
import { buildMetadata } from "@/lib/seo";

export const metadata = buildMetadata({
  title: "Enterprise",
  description:
    "Fleet management, role-based access control, high availability, disaster recovery, and compliance for organizations standardizing on LocalHost.",
  path: "/enterprise",
});

const stats = [
  { value: "10,000", label: "Hosts per control plane" },
  { value: "< 90s", label: "Typical HA failover time" },
  { value: "7 yrs", label: "Max audit log retention" },
  { value: "24/7/365", label: "Enterprise support" },
];

export default function EnterprisePage() {
  return (
    <div className="pb-24">
      <section className="border-b border-border bg-muted/20">
        <div className="container py-16 sm:py-20">
          <Breadcrumbs items={[{ title: "Enterprise" }]} />
          <h1 className="mt-6 max-w-2xl text-balance text-4xl font-semibold tracking-tight sm:text-5xl">
            Virtual infrastructure for the whole organization
          </h1>
          <p className="mt-4 max-w-2xl text-lg text-muted-foreground">
            LocalHost Enterprise gives IT and platform teams a single control plane for fleet
            management, security, and compliance — across on-premises data centers, with hybrid cloud
            integration on the roadmap.
          </p>
          <div className="mt-8 flex flex-wrap gap-3">
            <Button size="lg" asChild>
              <Link href="/contact">
                Talk to sales
                <ArrowRight className="h-4 w-4" />
              </Link>
            </Button>
            <Button size="lg" variant="outline" asChild>
              <Link href="/products/enterprise">View LocalHost Enterprise product</Link>
            </Button>
          </div>
        </div>
      </section>

      <section className="container py-16">
        <StatBand stats={stats} />
      </section>

      <section className="container py-8">
        <SectionHeading
          eyebrow="Capabilities"
          title="Built for IT and platform teams"
          description="Every capability below ships as part of LocalHost Enterprise and integrates with your existing identity, compliance, and automation tooling."
          className="mb-12"
        />
        <FeatureGrid items={enterpriseCapabilities} columns={3} />
      </section>

      <section className="border-t border-border bg-muted/20 py-20">
        <div className="container">
          <SectionHeading
            eyebrow="Rollout"
            title="How organizations deploy LocalHost Enterprise"
            description="A phased approach that validates fit before committing to a fleet-wide migration."
            className="mb-12"
          />
          <div className="mx-auto max-w-2xl">
            <Timeline steps={deploymentTimeline} />
          </div>
        </div>
      </section>

      <section className="container py-20 text-center">
        <h2 className="text-balance text-3xl font-semibold tracking-tight">
          Looking for hybrid cloud integration?
        </h2>
        <p className="mx-auto mt-3 max-w-xl text-muted-foreground">
          Native connectivity to public cloud environments is on the LocalHost roadmap. Contact our
          team to discuss timelines for your environment.
        </p>
        <Button className="mt-6" asChild>
          <Link href="/contact">
            Contact Sales
            <ArrowRight className="h-4 w-4" />
          </Link>
        </Button>
      </section>
    </div>
  );
}

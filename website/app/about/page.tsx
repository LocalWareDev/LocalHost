import type { Metadata } from "next";
import { Breadcrumbs } from "@/components/shared/breadcrumbs";
import { SectionHeading } from "@/components/shared/section-heading";
import { StatBand } from "@/components/shared/stat-band";
import { FeatureGrid } from "@/components/shared/feature-grid";
import { Compass, Handshake, ShieldCheck, Sparkles } from "lucide-react";

export const metadata: Metadata = {
  title: "About",
  description: "LocalWare Corporation builds LocalHost, an enterprise virtualization platform trusted by organizations worldwide.",
};

const stats = [
  { value: "2014", label: "Founded" },
  { value: "480+", label: "Employees" },
  { value: "60+", label: "Countries served" },
  { value: "2.4", label: "Current major release" },
];

const values = [
  { title: "Reliability first", description: "We ship conservatively. Production infrastructure doesn't get to be a beta test.", icon: ShieldCheck },
  { title: "Transparent engineering", description: "Release notes, security advisories, and architecture decisions are published openly.", icon: Compass },
  { title: "Customer-defined roadmap", description: "Enterprise capabilities are built from direct operator feedback, not speculation.", icon: Handshake },
  { title: "Performance as a feature", description: "We treat throughput and latency as product requirements, not optimizations for later.", icon: Sparkles },
];

export default function AboutPage() {
  return (
    <div className="pb-24">
      <section className="border-b border-border bg-muted/20">
        <div className="container py-16 sm:py-20">
          <Breadcrumbs items={[{ title: "About" }]} />
          <h1 className="mt-6 max-w-2xl text-balance text-4xl font-semibold tracking-tight sm:text-5xl">
            The company behind LocalHost
          </h1>
          <p className="mt-4 max-w-2xl text-lg text-muted-foreground">
            LocalWare Corporation builds virtualization infrastructure for organizations that treat
            reliability, security, and performance as non-negotiable requirements — not aspirations.
          </p>
        </div>
      </section>

      <section className="container py-16">
        <StatBand stats={stats} />
      </section>

      <section className="container py-8">
        <SectionHeading
          eyebrow="Mission"
          title="Virtualization infrastructure organizations can build on for a decade"
          description="We started LocalWare after watching engineering teams get burned by virtualization platforms that changed direction, deprecated APIs, or simply couldn't scale past a certain fleet size. LocalHost is built for organizations planning their infrastructure in years, not sprints."
          className="mb-4 max-w-3xl"
        />
      </section>

      <section className="border-t border-border bg-muted/20 py-20">
        <div className="container">
          <SectionHeading eyebrow="Values" title="What guides how we build" className="mb-12" />
          <FeatureGrid items={values} columns={4} />
        </div>
      </section>
    </div>
  );
}

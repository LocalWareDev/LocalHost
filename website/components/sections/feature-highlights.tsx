import { Cpu, GitBranch, Layers, Lock, Network, RefreshCw } from "lucide-react";
import { SectionHeading } from "@/components/shared/section-heading";
import { FeatureGrid } from "@/components/shared/feature-grid";

const features = [
  { title: "Near-native performance", description: "Paravirtualized I/O keeps throughput within single-digit percent of bare metal.", icon: Cpu },
  { title: "Tree-based snapshots", description: "Branch, compare, and roll back VM state without losing other snapshot paths.", icon: GitBranch },
  { title: "Software-defined storage", description: "Replicated, erasure-coded storage pools without dedicated SAN hardware.", icon: Layers },
  { title: "Defense-in-depth security", description: "Secure boot, encrypted disks, and isolated VM memory by default.", icon: Lock },
  { title: "Flexible networking", description: "NAT, bridged, host-only, and distributed virtual switches with VLAN tagging.", icon: Network },
  { title: "Live migration", description: "Move running workloads between hosts with sub-second downtime.", icon: RefreshCw },
];

export function FeatureHighlights() {
  return (
    <section className="border-y border-border bg-muted/20 py-20">
      <div className="container">
        <SectionHeading
          eyebrow="Platform"
          title="Everything a production virtualization platform should be"
          description="LocalHost is built around the fundamentals: performance you can measure, security you can audit, and operations you can automate."
          className="mb-12"
        />
        <FeatureGrid items={features} />
      </div>
    </section>
  );
}

import { FileCheck2, KeyRound, ScanEye, ShieldCheck } from "lucide-react";
import { SectionHeading } from "@/components/shared/section-heading";
import { FeatureGrid } from "@/components/shared/feature-grid";

const items = [
  { title: "Secure boot & attestation", description: "A verified boot chain with remote attestation for hypervisor hosts.", icon: ShieldCheck },
  { title: "Encrypted virtual disks", description: "At-rest encryption for VM disks with key management integration.", icon: KeyRound },
  { title: "Continuous audit logging", description: "Every administrative action is logged and exportable for review.", icon: ScanEye },
  { title: "Compliance frameworks", description: "Controls mapped to SOC 2, ISO 27001, and HIPAA requirements.", icon: FileCheck2 },
];

export function SecuritySection() {
  return (
    <section className="border-y border-border bg-muted/20 py-20">
      <div className="container">
        <SectionHeading
          eyebrow="Security"
          title="Security that holds up to an audit"
          description="LocalHost is designed around the assumption that it will be audited — not just used."
          className="mb-12"
        />
        <FeatureGrid items={items} columns={4} />
      </div>
    </section>
  );
}

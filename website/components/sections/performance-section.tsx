import { SectionHeading } from "@/components/shared/section-heading";
import { StatBand } from "@/components/shared/stat-band";

const stats = [
  { value: "< 1s", label: "Live migration downtime" },
  { value: "99.99%", label: "Typical fleet uptime SLA" },
  { value: "1,024", label: "Max VMs per hypervisor host" },
  { value: "12 TB", label: "Max memory per host" },
];

export function PerformanceSection() {
  return (
    <section className="container py-20">
      <SectionHeading
        eyebrow="Performance"
        title="Engineered for throughput, not just capacity"
        description="Paravirtualized drivers, NUMA-aware scheduling, and a minimal hypervisor footprint mean performance holds under real production load — not just in benchmarks."
        className="mb-12"
      />
      <StatBand stats={stats} />
    </section>
  );
}

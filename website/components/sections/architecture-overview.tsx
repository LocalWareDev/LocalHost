import { SectionHeading } from "@/components/shared/section-heading";

const layers = [
  {
    name: "Management Plane",
    description: "Fleet inventory, RBAC, policy engine, REST API, and Terraform provider.",
  },
  {
    name: "Orchestration & Scheduling",
    description: "NUMA-aware placement, live migration, high availability, and disaster recovery orchestration.",
  },
  {
    name: "Virtual Infrastructure",
    description: "Compute (vCPU/memory), software-defined storage, and virtual networking.",
  },
  {
    name: "LocalHost Hypervisor",
    description: "Minimal, hardened type-1 hypervisor with direct hardware access and secure boot.",
  },
  {
    name: "Physical Infrastructure",
    description: "Certified server hardware — CPU, memory, NVMe/SSD, and networking.",
  },
];

export function ArchitectureOverview() {
  return (
    <section className="container py-20">
      <SectionHeading
        eyebrow="Architecture"
        title="A layered architecture built for isolation"
        description="Each layer has a single responsibility, which keeps the platform auditable and makes failures easy to isolate and diagnose."
        className="mb-12"
      />
      <div className="mx-auto max-w-3xl space-y-3">
        {layers.map((layer, i) => (
          <div
            key={layer.name}
            className="flex flex-col gap-1 rounded-lg border border-border bg-card p-5 sm:flex-row sm:items-center sm:gap-6"
          >
            <div className="flex items-center gap-3 sm:w-56 sm:shrink-0">
              <span className="flex h-7 w-7 shrink-0 items-center justify-center rounded-full bg-primary/10 text-xs font-semibold text-primary">
                {layers.length - i}
              </span>
              <span className="font-semibold">{layer.name}</span>
            </div>
            <p className="text-sm text-muted-foreground">{layer.description}</p>
          </div>
        ))}
      </div>
    </section>
  );
}

export interface Capability {
  title: string;
  description: string;
}

export const enterpriseCapabilities: Capability[] = [
  {
    title: "Fleet management",
    description: "Manage every host and virtual machine across every site from a single, centralized console with real-time inventory and health visibility.",
  },
  {
    title: "Role-based access control",
    description: "Assign granular permissions at the organization, resource pool, or individual VM level, backed by full audit trails for every action.",
  },
  {
    title: "Virtual infrastructure",
    description: "Software-defined compute, storage, and networking that scales from a single rack to a global multi-site footprint.",
  },
  {
    title: "Centralized management",
    description: "A single control plane governs configuration, patching, and lifecycle policy across the entire fleet, eliminating host-by-host drift.",
  },
  {
    title: "Automation",
    description: "Policy-driven automation for provisioning, patching, snapshot retention, and resource quotas, scriptable via CLI, REST API, or Terraform.",
  },
  {
    title: "Compliance",
    description: "Continuous audit logging and exportable reporting mapped to SOC 2, ISO 27001, and HIPAA control frameworks.",
  },
  {
    title: "High availability",
    description: "Automatic VM failover across hosts with configurable recovery objectives, tested under real hardware failure scenarios.",
  },
  {
    title: "Disaster recovery",
    description: "Orchestrated failover to secondary sites with policy-based replication and recovery runbooks built into the management plane.",
  },
  {
    title: "Cloud integration",
    description: "Hybrid connectivity to public cloud environments is on the LocalHost roadmap, extending fleet management beyond on-premises infrastructure.",
  },
];

export const deploymentTimeline = [
  {
    phase: "Assessment",
    description: "LocalHost solutions architects review your current environment, workload profile, and compliance requirements.",
  },
  {
    phase: "Pilot",
    description: "A scoped deployment on a subset of hosts validates performance, integration, and operational fit before wider rollout.",
  },
  {
    phase: "Fleet rollout",
    description: "Phased migration across sites, orchestrated through the management plane with policy templates from the pilot.",
  },
  {
    phase: "Steady state",
    description: "Ongoing management through centralized policy, with your dedicated technical account manager for escalations and planning.",
  },
];

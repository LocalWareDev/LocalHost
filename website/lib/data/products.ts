export interface ProductFeature {
  title: string;
  description: string;
}

export interface ProductSpec {
  label: string;
  value: string;
}

export interface SystemRequirement {
  platform: string;
  cpu: string;
  memory: string;
  storage: string;
  notes: string;
}

export interface DownloadArtifact {
  platform: string;
  format: string;
  size: string;
}

export interface Product {
  slug: "workstation" | "enterprise" | "hypervisor" | "education";
  name: string;
  shortName: string;
  tagline: string;
  description: string;
  longDescription: string;
  audience: string;
  features: ProductFeature[];
  specs: ProductSpec[];
  licensing: {
    model: string;
    details: string[];
  };
  requirements: SystemRequirement[];
  downloads: DownloadArtifact[];
}

export const products: Product[] = [
  {
    slug: "workstation",
    name: "LocalHost Workstation",
    shortName: "Workstation",
    tagline: "Local virtualization for engineers who can't afford surprises.",
    description:
      "Run multiple isolated operating systems on a single machine with near-native performance, built for developers, QA teams, and power users.",
    longDescription:
      "LocalHost Workstation brings enterprise-grade virtualization to the desktop. Engineers can spin up isolated Linux, Windows, and BSD environments in seconds, snapshot state before risky changes, and reproduce production conditions locally — without touching a shared cluster or waiting on infrastructure tickets.",
    audience: "Individual engineers, QA teams, and technical consultants.",
    features: [
      { title: "Instant snapshots", description: "Capture full VM state before any change and roll back in seconds, with no downtime penalty." },
      { title: "Native-speed I/O", description: "Paravirtualized disk and network drivers keep throughput within single-digit percent of bare metal." },
      { title: "Unified VM library", description: "Organize, tag, and search machines across projects from a single local catalog." },
      { title: "Cross-platform host support", description: "Runs identically on Windows, Linux, and macOS hosts with the same feature set." },
      { title: "Shared folders & clipboard", description: "Move files and text between host and guest without configuring a network share." },
      { title: "Scriptable automation", description: "Drive VM lifecycle from the LocalHost CLI or REST API inside CI pipelines." },
    ],
    specs: [
      { label: "Max virtual CPUs per VM", value: "32 vCPU" },
      { label: "Max memory per VM", value: "128 GB" },
      { label: "Supported guest OS", value: "Windows, Linux, BSD" },
      { label: "Snapshot depth", value: "Unlimited, tree-based" },
      { label: "Virtual disk formats", value: "LHV, VHDX, QCOW2 (import)" },
      { label: "Networking modes", value: "NAT, bridged, host-only, isolated" },
    ],
    licensing: {
      model: "Per-seat, annual or perpetual",
      details: [
        "Single named-user license per install",
        "Includes minor version updates for the license term",
        "Volume discounts available above 25 seats",
      ],
    },
    requirements: [
      { platform: "Windows", cpu: "64-bit, VT-x/AMD-V with EPT/RVI", memory: "8 GB minimum, 16 GB recommended", storage: "20 GB free + guest disks", notes: "Windows 10 21H2 or later" },
      { platform: "macOS", cpu: "Apple Silicon or Intel with VT-x", memory: "8 GB minimum, 16 GB recommended", storage: "20 GB free + guest disks", notes: "macOS 13 Ventura or later" },
      { platform: "Linux", cpu: "64-bit with KVM support", memory: "8 GB minimum, 16 GB recommended", storage: "20 GB free + guest disks", notes: "Kernel 5.10+, glibc 2.31+" },
    ],
    downloads: [
      { platform: "Windows", format: ".exe installer", size: "184 MB" },
      { platform: "macOS (Apple Silicon)", format: ".dmg", size: "176 MB" },
      { platform: "macOS (Intel)", format: ".dmg", size: "179 MB" },
      { platform: "Linux", format: ".AppImage / .deb / .rpm", size: "168 MB" },
    ],
  },
  {
    slug: "enterprise",
    name: "LocalHost Enterprise",
    shortName: "Enterprise",
    tagline: "Fleet-scale virtual infrastructure for the whole organization.",
    description:
      "Centralized management, role-based access, and automated compliance across every virtual machine your organization runs — on-prem or across data centers.",
    longDescription:
      "LocalHost Enterprise extends the LocalHost platform with a control plane built for IT and platform teams: centralized fleet management, granular role-based access control, policy-driven automation, and the audit trail regulated industries require. It is the foundation for organizations standardizing virtualization across hundreds or thousands of endpoints.",
    audience: "IT operations, platform engineering, and infrastructure teams.",
    features: [
      { title: "Centralized fleet console", description: "Manage every host and VM in the organization from a single pane of glass." },
      { title: "Role-based access control", description: "Fine-grained permissions down to the individual VM, resource pool, or data center." },
      { title: "Policy-driven automation", description: "Enforce patching, snapshot retention, and resource quotas automatically across fleets." },
      { title: "High availability", description: "Automatic VM failover across hosts with configurable recovery time objectives." },
      { title: "Compliance reporting", description: "Continuous audit logs and exportable reports aligned to SOC 2 and ISO 27001 controls." },
      { title: "Directory integration", description: "SSO and group sync with Active Directory, Okta, and any SAML/OIDC provider." },
    ],
    specs: [
      { label: "Managed hosts", value: "Up to 10,000 per control plane" },
      { label: "Concurrent VMs", value: "Unlimited (license-gated)" },
      { label: "RBAC granularity", value: "Organization / pool / VM" },
      { label: "HA failover time", value: "< 90 seconds typical" },
      { label: "Audit log retention", value: "Configurable, 7 years max" },
      { label: "API", value: "Full REST + Terraform provider" },
    ],
    licensing: {
      model: "Per-managed-host, annual subscription",
      details: [
        "Includes centralized management plane and updates",
        "Tiered support SLAs from business hours to 24/7/365",
        "Custom enterprise agreements for regulated industries",
      ],
    },
    requirements: [
      { platform: "Management plane", cpu: "8-core minimum", memory: "32 GB minimum", storage: "500 GB SSD, RAID recommended", notes: "Runs on LocalHost Hypervisor or supported Linux distributions" },
      { platform: "Managed hosts", cpu: "64-bit with VT-x/AMD-V and IOMMU", memory: "32 GB minimum per host", storage: "Enterprise SSD/NVMe recommended", notes: "See Hypervisor requirements for bare-metal nodes" },
    ],
    downloads: [
      { platform: "Management Plane (Linux)", format: ".iso / .tar.gz", size: "612 MB" },
      { platform: "Fleet Agent (Windows)", format: ".msi", size: "94 MB" },
      { platform: "Fleet Agent (Linux)", format: ".deb / .rpm", size: "88 MB" },
    ],
  },
  {
    slug: "hypervisor",
    name: "LocalHost Hypervisor",
    shortName: "Hypervisor",
    tagline: "Bare-metal type-1 virtualization for production workloads.",
    description:
      "A dedicated, minimal-footprint hypervisor installed directly on server hardware, delivering maximum throughput and isolation for production environments.",
    longDescription:
      "LocalHost Hypervisor is a type-1 bare-metal hypervisor purpose-built for production infrastructure. With a minimal, hardened base and direct hardware access, it delivers the throughput, isolation, and predictability that latency-sensitive and regulated workloads require — while integrating natively with LocalHost Enterprise for fleet-wide management.",
    audience: "Data center operators, service providers, and production infrastructure teams.",
    features: [
      { title: "Minimal attack surface", description: "A hardened, purpose-built base with no general-purpose OS overhead." },
      { title: "Direct hardware access", description: "PCI passthrough and SR-IOV support for GPU and NIC-intensive workloads." },
      { title: "Live migration", description: "Move running VMs between hosts with zero downtime for maintenance or rebalancing." },
      { title: "NUMA-aware scheduling", description: "Automatic CPU and memory placement tuned to modern multi-socket servers." },
      { title: "Software-defined storage", description: "Distributed, replicated storage pools across hosts without external SAN hardware." },
      { title: "Secure boot & attestation", description: "Verified boot chain with remote attestation for compliance-sensitive deployments." },
    ],
    specs: [
      { label: "Max physical CPUs per host", value: "2 sockets, up to 224 cores" },
      { label: "Max memory per host", value: "12 TB" },
      { label: "Max VMs per host", value: "1,024" },
      { label: "Live migration downtime", value: "< 1 second typical" },
      { label: "Storage architecture", value: "Distributed, erasure-coded" },
      { label: "Footprint", value: "< 350 MB installed base" },
    ],
    licensing: {
      model: "Per-CPU-socket, annual subscription",
      details: [
        "Unlimited VMs per licensed host",
        "Includes security patches for the license term",
        "OEM and service-provider licensing available",
      ],
    },
    requirements: [
      { platform: "Server hardware", cpu: "64-bit x86, VT-x/AMD-V, EPT/RVI, IOMMU", memory: "64 GB minimum, ECC recommended", storage: "Dedicated boot device + NVMe/SSD pool", notes: "Certified hardware list available in Documentation" },
    ],
    downloads: [
      { platform: "Installer ISO", format: ".iso", size: "340 MB" },
      { platform: "PXE Boot Image", format: ".img", size: "336 MB" },
    ],
  },
  {
    slug: "education",
    name: "LocalHost Education",
    shortName: "Education",
    tagline: "Classroom and lab environments, provisioned in minutes.",
    description:
      "Pre-configured virtual lab environments for computer science and IT programs, with simplified licensing and classroom management tools.",
    longDescription:
      "LocalHost Education brings the same virtualization engine used in production data centers into the classroom. Instructors provision identical lab environments for every student in minutes, reset them between sessions, and manage cohorts centrally — with academic pricing that makes fleet-wide deployment realistic for institutions.",
    audience: "Universities, bootcamps, and IT training programs.",
    features: [
      { title: "Classroom templates", description: "Distribute identical, pre-configured VM images to an entire cohort in one action." },
      { title: "Session reset", description: "Return every lab machine to a known-good state between classes automatically." },
      { title: "Cohort management", description: "Group students by class or section with scoped access to shared lab pools." },
      { title: "Low-resource mode", description: "Optimized defaults for shared lab hardware and constrained student devices." },
      { title: "Offline lab packs", description: "Distribute self-contained lab exercises that run without an internet connection." },
      { title: "Academic reporting", description: "Usage and completion visibility for lab-based coursework." },
    ],
    specs: [
      { label: "Max seats per site license", value: "Unlimited (institution-wide)" },
      { label: "Template distribution", value: "One-to-many, versioned" },
      { label: "Guest OS support", value: "Windows, Linux, BSD" },
      { label: "Lab reset time", value: "< 30 seconds typical" },
      { label: "Offline support", value: "Full offline lab mode" },
    ],
    licensing: {
      model: "Institutional site license, annual",
      details: [
        "Priced per institution, not per seat",
        "Includes classroom management console",
        "Free for accredited coursework under the LocalHost Academic Program",
      ],
    },
    requirements: [
      { platform: "Student workstation", cpu: "64-bit with VT-x/AMD-V", memory: "8 GB minimum", storage: "20 GB free + guest disks", notes: "Windows, macOS, or Linux" },
      { platform: "Lab management server", cpu: "8-core minimum", memory: "16 GB minimum", storage: "500 GB for template library", notes: "Optional; required for centrally managed labs" },
    ],
    downloads: [
      { platform: "Student Edition (Windows)", format: ".exe installer", size: "182 MB" },
      { platform: "Student Edition (macOS)", format: ".dmg", size: "175 MB" },
      { platform: "Student Edition (Linux)", format: ".AppImage", size: "166 MB" },
      { platform: "Lab Management Console", format: ".iso", size: "420 MB" },
    ],
  },
];

export function getProductBySlug(slug: string) {
  return products.find((p) => p.slug === slug);
}

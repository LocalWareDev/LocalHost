export interface DocBlock {
  heading: string;
  body: string[];
  code?: { label: string; content: string };
}

export interface DocPage {
  slug: string;
  title: string;
  description: string;
  blocks: DocBlock[];
}

export interface DocSection {
  title: string;
  pages: DocPage[];
}

export const docSections: DocSection[] = [
  {
    title: "Getting Started",
    pages: [
      {
        slug: "quick-start",
        title: "Quick Start",
        description: "Get a virtual machine running with LocalHost in under five minutes.",
        blocks: [
          {
            heading: "Before you begin",
            body: [
              "Confirm your host meets the minimum requirements listed in System Requirements. Virtualization extensions (VT-x/AMD-V) must be enabled in firmware.",
              "Download the appropriate installer for your platform from the Download Center.",
            ],
          },
          {
            heading: "Create your first VM",
            body: [
              "Launch LocalHost and select New Virtual Machine. Choose a guest OS family, allocate CPU and memory, and point to an installation image.",
              "LocalHost applies sensible defaults for the selected guest OS automatically, including recommended disk size and network mode.",
            ],
            code: {
              label: "CLI equivalent",
              content: "localhost vm create \\\n  --name dev-ubuntu \\\n  --os linux-generic \\\n  --cpus 4 \\\n  --memory 8G \\\n  --disk 60G \\\n  --iso ./ubuntu-24.04.iso",
            },
          },
          {
            heading: "Boot and connect",
            body: [
              "Start the VM from the library view or with `localhost vm start dev-ubuntu`. The integrated console attaches automatically; SPICE and VNC are also supported for remote access.",
            ],
          },
        ],
      },
      {
        slug: "installation",
        title: "Installation",
        description: "Platform-specific installation steps and silent install options for fleet deployment.",
        blocks: [
          {
            heading: "Windows",
            body: [
              "Run the .exe installer as an administrator. For unattended deployment across a fleet, use the silent install flags below with your existing software distribution tooling.",
            ],
            code: { label: "Silent install", content: "LocalHost-Setup.exe /S /LICENSE=ENTERPRISE /CONFIG=C:\\deploy\\localhost.json" },
          },
          {
            heading: "macOS",
            body: [
              "Mount the .dmg and drag LocalHost to Applications. Notarization and Gatekeeper checks are handled automatically.",
              "For MDM-based deployment, a .pkg installer is available in the Enterprise download bundle.",
            ],
          },
          {
            heading: "Linux",
            body: [
              "Debian and RPM packages are provided for apt and dnf-based distributions. An AppImage is available for distribution-agnostic installs.",
            ],
            code: { label: "Debian/Ubuntu", content: "sudo apt install ./localhost_2.4.0_amd64.deb" },
          },
        ],
      },
    ],
  },
  {
    title: "Core Concepts",
    pages: [
      {
        slug: "creating-virtual-machines",
        title: "Creating Virtual Machines",
        description: "Provisioning options, templates, and guest OS optimization.",
        blocks: [
          {
            heading: "From a template",
            body: [
              "Templates capture a fully configured VM — OS, patches, and applications — as a reusable source. Cloning from a template takes seconds and uses copy-on-write disks to minimize storage overhead.",
            ],
          },
          {
            heading: "Guest tools",
            body: [
              "Install LocalHost Guest Tools inside the VM for optimized drivers, time synchronization, dynamic display resizing, and host-guest clipboard sharing.",
            ],
          },
        ],
      },
      {
        slug: "snapshots",
        title: "Snapshots",
        description: "Point-in-time VM state capture, branching, and rollback.",
        blocks: [
          {
            heading: "Taking a snapshot",
            body: [
              "Snapshots capture disk, memory, and device state. They are tree-structured, so you can branch from any prior snapshot without losing other branches.",
            ],
            code: { label: "CLI", content: "localhost snapshot create dev-ubuntu --name pre-upgrade" },
          },
          {
            heading: "Reverting",
            body: [
              "Reverting to a snapshot is near-instant for powered-off VMs and takes only slightly longer for running VMs, since memory state is restored alongside disk state.",
            ],
          },
        ],
      },
      {
        slug: "networking",
        title: "Networking",
        description: "Virtual network modes and configuration for isolated or connected topologies.",
        blocks: [
          {
            heading: "Network modes",
            body: [
              "NAT provides outbound connectivity with no inbound exposure, ideal for most development use. Bridged mode places the VM directly on the physical network with its own address. Host-only and isolated modes are available for air-gapped testing.",
            ],
          },
          {
            heading: "Virtual switches (Enterprise)",
            body: [
              "LocalHost Enterprise adds software-defined virtual switches with VLAN tagging, distributed across hosts, so VM networking policy travels with live migration.",
            ],
          },
        ],
      },
      {
        slug: "storage",
        title: "Storage",
        description: "Disk formats, storage pools, and distributed storage for fleet deployments.",
        blocks: [
          {
            heading: "Virtual disk formats",
            body: [
              "LocalHost's native LHV format supports thin provisioning, compression, and encryption. VHDX and QCOW2 images can be imported directly without conversion.",
            ],
          },
          {
            heading: "Distributed storage (Hypervisor)",
            body: [
              "LocalHost Hypervisor pools NVMe and SSD storage across hosts into a replicated, erasure-coded volume, removing the need for external SAN hardware in most deployments.",
            ],
          },
        ],
      },
      {
        slug: "performance-tuning",
        title: "Performance Tuning",
        description: "CPU, memory, and I/O tuning guidance for latency-sensitive workloads.",
        blocks: [
          {
            heading: "CPU pinning and NUMA",
            body: [
              "For latency-sensitive workloads, pin vCPUs to physical cores and align VM memory allocation with a single NUMA node to avoid cross-node memory access penalties.",
            ],
          },
          {
            heading: "Paravirtualized drivers",
            body: [
              "Always install LocalHost Guest Tools for paravirtualized disk and network drivers — emulated device fallbacks exist for compatibility but cost significant throughput.",
            ],
          },
        ],
      },
    ],
  },
  {
    title: "Reference",
    pages: [
      {
        slug: "cli-reference",
        title: "CLI Reference",
        description: "Common LocalHost CLI commands for scripting and automation.",
        blocks: [
          {
            heading: "VM lifecycle",
            body: ["Core commands for creating, starting, stopping, and removing virtual machines."],
            code: {
              label: "Common commands",
              content:
                "localhost vm list\nlocalhost vm start <name>\nlocalhost vm stop <name> [--force]\nlocalhost vm delete <name>\nlocalhost snapshot list <name>\nlocalhost snapshot revert <name> <snapshot>",
            },
          },
          {
            heading: "Fleet commands (Enterprise)",
            body: ["Available when connected to a LocalHost Enterprise management plane."],
            code: {
              label: "Fleet commands",
              content: "localhost fleet hosts list\nlocalhost fleet policy apply --pool prod-east\nlocalhost fleet report compliance --format pdf",
            },
          },
        ],
      },
      {
        slug: "rest-api",
        title: "REST API",
        description: "Programmatic access to VM lifecycle and fleet operations.",
        blocks: [
          {
            heading: "Authentication",
            body: [
              "The LocalHost API uses bearer tokens issued from the management console or CLI. Tokens can be scoped to read-only or full-control access.",
            ],
            code: {
              label: "Example request",
              content: "curl https://api.localhost.localware.com/v1/vms \\\n  -H \"Authorization: Bearer $LOCALHOST_TOKEN\"",
            },
          },
          {
            heading: "Status",
            body: [
              "Full endpoint reference, including fleet and compliance endpoints, ships with LocalHost Enterprise and is published to your management console at /api/docs on activation.",
            ],
          },
        ],
      },
      {
        slug: "faq",
        title: "FAQ",
        description: "Answers to common questions about licensing, compatibility, and support.",
        blocks: [
          {
            heading: "Can I move a VM between LocalHost products?",
            body: ["Yes. VMs created in Workstation can be imported into Enterprise or Hypervisor environments without conversion."],
          },
          {
            heading: "Does LocalHost support nested virtualization?",
            body: ["Yes, on hosts where the physical CPU exposes nested VT-x/AMD-V extensions. See Performance Tuning for guidance."],
          },
          {
            heading: "How are security patches delivered?",
            body: ["Security updates are released out-of-band from feature releases and are documented in Release Notes with severity ratings."],
          },
        ],
      },
    ],
  },
];

export function getAllDocPages(): DocPage[] {
  return docSections.flatMap((s) => s.pages);
}

export function getDocPageBySlug(slug: string) {
  return getAllDocPages().find((p) => p.slug === slug);
}

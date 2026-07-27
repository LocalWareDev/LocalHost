export type BlogCategory = "Engineering" | "Release Notes" | "Security Advisories" | "Development" | "Performance";

export interface BlogPost {
  slug: string;
  title: string;
  excerpt: string;
  category: BlogCategory;
  author: string;
  date: string;
  readingTime: string;
  content: string[];
}

export const blogPosts: BlogPost[] = [
  {
    slug: "localhost-2-4-live-migration",
    title: "Zero-downtime live migration in LocalHost 2.4",
    excerpt: "How we rebuilt VM migration around a pre-copy memory algorithm to bring downtime under one second at scale.",
    category: "Engineering",
    author: "LocalHost Platform Team",
    date: "2026-06-18",
    readingTime: "7 min read",
    content: [
      "Live migration has been part of LocalHost Hypervisor since its first release, but the 2.4 rewrite changes the underlying algorithm from a fixed-round pre-copy to an adaptive model that watches page dirty rates in real time.",
      "The result is migration downtime that stays under one second for the overwhelming majority of production workloads we tested, including memory-intensive database instances that previously required a maintenance window.",
      "This post walks through the scheduler changes, the new dirty-page bitmap implementation, and the failure modes we hardened against during a six-month internal rollout.",
    ],
  },
  {
    slug: "release-notes-2-4-0",
    title: "LocalHost 2.4.0 Release Notes",
    excerpt: "Live migration improvements, RBAC scoping to individual VMs, and a refreshed CLI.",
    category: "Release Notes",
    author: "LocalHost Release Engineering",
    date: "2026-06-10",
    readingTime: "4 min read",
    content: [
      "LocalHost 2.4.0 is now generally available for Workstation, Enterprise, and Hypervisor. Highlights include sub-second live migration, VM-level RBAC scoping in Enterprise, and a rewritten CLI with faster startup and JSON output support.",
      "Upgrade paths from 2.2.x and 2.3.x are supported directly. Customers on 2.1.x or earlier should review the migration guide in Documentation before upgrading.",
    ],
  },
  {
    slug: "advisory-2026-07-snic-timing",
    title: "Security Advisory: timing side-channel in synthetic NIC driver",
    excerpt: "A low-severity timing side-channel affecting the paravirtualized network driver on Hypervisor 2.3 and earlier.",
    category: "Security Advisories",
    author: "LocalHost Security Team",
    date: "2026-07-02",
    readingTime: "3 min read",
    content: [
      "We were notified of a timing side-channel in the synthetic NIC driver that could, under specific conditions, allow a co-located VM to infer coarse-grained network activity of a neighboring VM.",
      "The issue is rated low severity (CVSS 3.7) and has been fixed in Hypervisor 2.4.0 and backported to 2.3.2. Customers running affected versions should patch at the next available maintenance window; there is no evidence of exploitation in the wild.",
    ],
  },
  {
    slug: "terraform-provider-ga",
    title: "The LocalHost Terraform provider is now generally available",
    excerpt: "Declarative VM and fleet-policy management for teams already standardized on infrastructure as code.",
    category: "Development",
    author: "LocalHost Developer Relations",
    date: "2026-05-22",
    readingTime: "5 min read",
    content: [
      "Teams running LocalHost Enterprise alongside existing Terraform-managed infrastructure can now describe VM fleets, resource pools, and RBAC policy as code.",
      "The provider supports import of existing VMs, drift detection against the management plane, and a policy module for common compliance baselines.",
    ],
  },
  {
    slug: "numa-aware-scheduling-benchmarks",
    title: "Benchmarking NUMA-aware scheduling on dual-socket servers",
    excerpt: "A look at throughput gains from NUMA-aware placement on database and in-memory cache workloads.",
    category: "Performance",
    author: "LocalHost Platform Team",
    date: "2026-04-30",
    readingTime: "8 min read",
    content: [
      "NUMA-aware scheduling in LocalHost Hypervisor keeps a VM's vCPUs and memory allocation on the same physical socket whenever capacity allows, avoiding cross-node memory access latency.",
      "On our reference dual-socket benchmark suite, workloads sensitive to memory latency saw throughput improvements of 15-30% once NUMA-aware placement was enabled, with the largest gains on in-memory cache and OLTP database workloads.",
    ],
  },
  {
    slug: "distributed-storage-architecture",
    title: "Inside LocalHost's distributed storage architecture",
    excerpt: "How erasure coding and replication work together to remove the need for a dedicated SAN.",
    category: "Engineering",
    author: "LocalHost Platform Team",
    date: "2026-03-14",
    readingTime: "9 min read",
    content: [
      "LocalHost Hypervisor's distributed storage pools NVMe and SSD capacity across every host in a cluster, combining replication for hot data with erasure coding for cold data to balance durability against usable capacity.",
      "This post covers the placement algorithm, rebuild behavior after a host failure, and the trade-offs we made to keep write latency predictable under rebuild load.",
    ],
  },
];

export const blogCategories: BlogCategory[] = [
  "Engineering",
  "Release Notes",
  "Security Advisories",
  "Development",
  "Performance",
];

export function getPostBySlug(slug: string) {
  return blogPosts.find((p) => p.slug === slug);
}

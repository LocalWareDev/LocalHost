export interface PricingPlan {
  id: "community" | "professional" | "enterprise";
  name: string;
  price: string;
  priceDetail: string;
  description: string;
  cta: string;
  href: string;
  highlighted?: boolean;
  features: string[];
}

export const pricingPlans: PricingPlan[] = [
  {
    id: "community",
    name: "Community",
    price: "$0",
    priceDetail: "forever, single user",
    description: "For individual engineers evaluating LocalHost or running personal projects.",
    cta: "Download Community",
    href: "/download",
    features: [
      "LocalHost Workstation, 1 seat",
      "Up to 4 concurrent VMs",
      "Community support forum",
      "Standard snapshotting",
      "Local VM library",
    ],
  },
  {
    id: "professional",
    name: "Professional",
    price: "$29",
    priceDetail: "per seat / month, billed annually",
    description: "For teams that need advanced features and direct vendor support.",
    cta: "Start Professional",
    href: "/contact",
    highlighted: true,
    features: [
      "Everything in Community",
      "Unlimited concurrent VMs",
      "Priority email support, 1 business day SLA",
      "Advanced networking modes",
      "REST API & CLI automation",
      "Shared team VM libraries",
    ],
  },
  {
    id: "enterprise",
    name: "Enterprise",
    price: "Custom",
    priceDetail: "per managed host, annual agreement",
    description: "For organizations standardizing virtualization across the fleet.",
    cta: "Contact Sales",
    href: "/contact",
    features: [
      "Everything in Professional",
      "LocalHost Enterprise fleet management",
      "Role-based access control",
      "High availability & disaster recovery",
      "SSO / directory integration",
      "24/7/365 support with dedicated TAM",
      "Compliance & audit reporting",
    ],
  },
];

export interface ComparisonRow {
  feature: string;
  community: string | boolean;
  professional: string | boolean;
  enterprise: string | boolean;
}

export const pricingComparison: { category: string; rows: ComparisonRow[] }[] = [
  {
    category: "Core virtualization",
    rows: [
      { feature: "Concurrent VMs", community: "4", professional: "Unlimited", enterprise: "Unlimited" },
      { feature: "Snapshots", community: true, professional: true, enterprise: true },
      { feature: "Shared folders & clipboard", community: true, professional: true, enterprise: true },
      { feature: "Advanced networking modes", community: false, professional: true, enterprise: true },
    ],
  },
  {
    category: "Automation & API",
    rows: [
      { feature: "LocalHost CLI", community: true, professional: true, enterprise: true },
      { feature: "REST API", community: false, professional: true, enterprise: true },
      { feature: "Terraform provider", community: false, professional: false, enterprise: true },
    ],
  },
  {
    category: "Management",
    rows: [
      { feature: "Fleet management console", community: false, professional: false, enterprise: true },
      { feature: "Role-based access control", community: false, professional: false, enterprise: true },
      { feature: "Directory / SSO integration", community: false, professional: false, enterprise: true },
      { feature: "High availability", community: false, professional: false, enterprise: true },
      { feature: "Disaster recovery orchestration", community: false, professional: false, enterprise: true },
    ],
  },
  {
    category: "Support",
    rows: [
      { feature: "Community forum", community: true, professional: true, enterprise: true },
      { feature: "Priority email support", community: false, professional: true, enterprise: true },
      { feature: "24/7/365 support", community: false, professional: false, enterprise: true },
      { feature: "Dedicated technical account manager", community: false, professional: false, enterprise: true },
    ],
  },
];

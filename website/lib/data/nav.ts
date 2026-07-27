export interface NavLink {
  title: string;
  href: string;
  description?: string;
}

export interface NavGroup {
  title: string;
  href?: string;
  items?: NavLink[];
}

export const mainNav: NavGroup[] = [
  {
    title: "Products",
    href: "/products",
    items: [
      {
        title: "LocalHost Workstation",
        href: "/products/workstation",
        description: "Local virtualization for engineers and power users.",
      },
      {
        title: "LocalHost Enterprise",
        href: "/products/enterprise",
        description: "Fleet-scale virtual infrastructure for the organization.",
      },
      {
        title: "LocalHost Hypervisor",
        href: "/products/hypervisor",
        description: "Bare-metal type-1 hypervisor for production workloads.",
      },
      {
        title: "LocalHost Education",
        href: "/products/education",
        description: "Classroom and lab environments for institutions.",
      },
    ],
  },
  {
    title: "Enterprise",
    href: "/enterprise",
  },
  {
    title: "Documentation",
    href: "/documentation",
  },
  {
    title: "Pricing",
    href: "/pricing",
  },
  {
    title: "Resources",
    items: [
      { title: "Download Center", href: "/download", description: "Stable, beta, and nightly builds." },
      { title: "Blog", href: "/blog", description: "Engineering notes and release updates." },
      { title: "Support", href: "/support", description: "Knowledge base and support channels." },
      { title: "About", href: "/about", description: "The company behind LocalHost." },
    ],
  },
];

export const footerNav = {
  product: [
    { title: "Workstation", href: "/products/workstation" },
    { title: "Enterprise", href: "/products/enterprise" },
    { title: "Hypervisor", href: "/products/hypervisor" },
    { title: "Education", href: "/products/education" },
    { title: "Pricing", href: "/pricing" },
  ],
  resources: [
    { title: "Documentation", href: "/documentation" },
    { title: "Download Center", href: "/download" },
    { title: "Blog", href: "/blog" },
    { title: "Support", href: "/support" },
    { title: "System Status", href: "/support#status" },
  ],
  company: [
    { title: "About", href: "/about" },
    { title: "Enterprise", href: "/enterprise" },
    { title: "Contact", href: "/contact" },
    { title: "Blog", href: "/blog" },
  ],
  legal: [
    { title: "Privacy Policy", href: "/privacy" },
    { title: "Terms of Service", href: "/terms" },
  ],
};

export const commandItems = [
  { title: "Home", href: "/", group: "Navigate" },
  { title: "Products", href: "/products", group: "Navigate" },
  { title: "Enterprise", href: "/enterprise", group: "Navigate" },
  { title: "Documentation", href: "/documentation", group: "Navigate" },
  { title: "Pricing", href: "/pricing", group: "Navigate" },
  { title: "Download Center", href: "/download", group: "Navigate" },
  { title: "Blog", href: "/blog", group: "Navigate" },
  { title: "Support", href: "/support", group: "Navigate" },
  { title: "About", href: "/about", group: "Navigate" },
  { title: "Contact", href: "/contact", group: "Navigate" },
  { title: "LocalHost Workstation", href: "/products/workstation", group: "Products" },
  { title: "LocalHost Enterprise", href: "/products/enterprise", group: "Products" },
  { title: "LocalHost Hypervisor", href: "/products/hypervisor", group: "Products" },
  { title: "LocalHost Education", href: "/products/education", group: "Products" },
  { title: "Quick Start Guide", href: "/documentation/quick-start", group: "Documentation" },
  { title: "Installation", href: "/documentation/installation", group: "Documentation" },
  { title: "CLI Reference", href: "/documentation/cli-reference", group: "Documentation" },
];

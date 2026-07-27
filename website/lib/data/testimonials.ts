export interface Testimonial {
  quote: string;
  name: string;
  title: string;
  company: string;
}

export const testimonials: Testimonial[] = [
  {
    quote:
      "We standardized every regional data center on LocalHost Hypervisor over eighteen months. Live migration downtime dropped from minutes to under a second, and our audit prep time was cut in half.",
    name: "Infrastructure Director",
    title: "VP, Infrastructure",
    company: "Multinational Financial Services Firm",
  },
  {
    quote:
      "The RBAC model is the reason we passed our SOC 2 audit without a single finding on access control. It maps cleanly to how our organization is actually structured.",
    name: "Platform Security Lead",
    title: "Head of Platform Security",
    company: "Enterprise SaaS Provider",
  },
  {
    quote:
      "Our engineering team moved off a shared virtualization cluster entirely. Local snapshots and near-native I/O performance mean nobody is waiting on a ticket to reproduce a production issue anymore.",
    name: "Engineering Manager",
    title: "Director of Engineering",
    company: "Cloud Infrastructure Company",
  },
];

export const customerLogos: string[] = [
  "Meridian Financial",
  "Northwind Systems",
  "Argent Health Networks",
  "Vantage Logistics",
  "Cobalt Research",
  "Fathom Data",
];

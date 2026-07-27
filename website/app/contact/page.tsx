import type { Metadata } from "next";
import { Breadcrumbs } from "@/components/shared/breadcrumbs";
import { ContactForm } from "@/components/forms/contact-form";
import { Building2, Mail, MapPin } from "lucide-react";

export const metadata: Metadata = {
  title: "Contact",
  description: "Talk to LocalWare sales, support, or partnerships teams.",
};

const details = [
  { icon: Mail, label: "Sales", value: "sales@localware.com" },
  { icon: Mail, label: "Support", value: "support@localware.com" },
  { icon: Building2, label: "Company", value: "LocalWare Corporation" },
  { icon: MapPin, label: "Headquarters", value: "San Francisco, CA · Remote-friendly" },
];

export default function ContactPage() {
  return (
    <div className="pb-24">
      <section className="border-b border-border bg-muted/20">
        <div className="container py-16">
          <Breadcrumbs items={[{ title: "Contact" }]} />
          <h1 className="mt-6 max-w-2xl text-balance text-4xl font-semibold tracking-tight sm:text-5xl">
            Talk to our team
          </h1>
          <p className="mt-4 max-w-2xl text-lg text-muted-foreground">
            Whether you're evaluating LocalHost for a single team or planning a fleet-wide rollout,
            we'll route your message to the right people.
          </p>
        </div>
      </section>

      <section className="container py-16">
        <div className="grid grid-cols-1 gap-12 lg:grid-cols-3">
          <div className="lg:col-span-2">
            <ContactForm />
          </div>
          <div className="space-y-6">
            {details.map((detail) => (
              <div key={detail.label} className="flex items-start gap-3">
                <div className="flex h-9 w-9 shrink-0 items-center justify-center rounded-md bg-primary/10 text-primary">
                  <detail.icon className="h-4 w-4" />
                </div>
                <div>
                  <p className="text-sm font-semibold">{detail.label}</p>
                  <p className="text-sm text-muted-foreground">{detail.value}</p>
                </div>
              </div>
            ))}
          </div>
        </div>
      </section>
    </div>
  );
}

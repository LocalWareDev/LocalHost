import { LogoCloud } from "@/components/shared/logo-cloud";
import { customerLogos } from "@/lib/data/testimonials";

export function CustomerLogos() {
  return (
    <section className="container py-16">
      <p className="text-center text-sm font-medium uppercase tracking-wider text-muted-foreground">
        Trusted by infrastructure teams at
      </p>
      <div className="mt-8">
        <LogoCloud names={customerLogos} />
      </div>
    </section>
  );
}

import Link from "next/link";
import { Check } from "lucide-react";
import { cn } from "@/lib/utils";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardHeader } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import type { PricingPlan } from "@/lib/data/pricing";

export function PricingCard({ plan }: { plan: PricingPlan }) {
  return (
    <Card
      className={cn(
        "relative flex h-full flex-col",
        plan.highlighted && "border-primary shadow-lg shadow-primary/10 ring-1 ring-primary"
      )}
    >
      {plan.highlighted && (
        <Badge className="absolute -top-3 left-1/2 -translate-x-1/2">Most popular</Badge>
      )}
      <CardHeader>
        <h3 className="text-lg font-semibold">{plan.name}</h3>
        <div className="mt-2 flex items-baseline gap-1">
          <span className="text-4xl font-semibold tracking-tight">{plan.price}</span>
        </div>
        <p className="text-sm text-muted-foreground">{plan.priceDetail}</p>
        <p className="mt-3 text-sm text-muted-foreground">{plan.description}</p>
      </CardHeader>
      <CardContent className="flex flex-1 flex-col justify-between">
        <ul className="space-y-3">
          {plan.features.map((feature) => (
            <li key={feature} className="flex items-start gap-2 text-sm">
              <Check className="mt-0.5 h-4 w-4 shrink-0 text-primary" />
              <span>{feature}</span>
            </li>
          ))}
        </ul>
        <Button asChild className="mt-8" variant={plan.highlighted ? "default" : "outline"}>
          <Link href={plan.href}>{plan.cta}</Link>
        </Button>
      </CardContent>
    </Card>
  );
}

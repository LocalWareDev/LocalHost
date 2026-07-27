"use client";

import { motion } from "framer-motion";
import { type LucideIcon } from "lucide-react";
import { Card, CardContent } from "@/components/ui/card";

export interface FeatureGridItem {
  title: string;
  description: string;
  icon?: LucideIcon;
}

export function FeatureGrid({ items, columns = 3 }: { items: FeatureGridItem[]; columns?: 2 | 3 | 4 }) {
  const colClass =
    columns === 2 ? "sm:grid-cols-2" : columns === 4 ? "sm:grid-cols-2 lg:grid-cols-4" : "sm:grid-cols-2 lg:grid-cols-3";

  return (
    <div className={`grid grid-cols-1 gap-5 ${colClass}`}>
      {items.map((item, i) => {
        const Icon = item.icon;
        return (
          <motion.div
            key={item.title}
            initial={{ opacity: 0, y: 12 }}
            whileInView={{ opacity: 1, y: 0 }}
            viewport={{ once: true, margin: "-60px" }}
            transition={{ duration: 0.4, delay: i * 0.04 }}
          >
            <Card className="h-full transition-colors hover:border-primary/40">
              <CardContent className="p-6">
                {Icon && (
                  <div className="mb-4 flex h-10 w-10 items-center justify-center rounded-md bg-primary/10 text-primary">
                    <Icon className="h-5 w-5" />
                  </div>
                )}
                <h3 className="font-semibold">{item.title}</h3>
                <p className="mt-2 text-sm text-muted-foreground">{item.description}</p>
              </CardContent>
            </Card>
          </motion.div>
        );
      })}
    </div>
  );
}

"use client";

import Link from "next/link";
import { motion } from "framer-motion";
import { ArrowRight, PlayCircle } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { ScreenshotPlaceholder } from "@/components/shared/screenshot-placeholder";

export function Hero() {
  return (
    <section className="relative overflow-hidden border-b border-border">
      <div className="bg-grid pointer-events-none absolute inset-0 [mask-image:radial-gradient(ellipse_60%_60%_at_50%_0%,#000_10%,transparent_70%)]" />
      <div className="container relative py-20 sm:py-28">
        <div className="mx-auto max-w-3xl text-center">
          <motion.div
            initial={{ opacity: 0, y: 10 }}
            animate={{ opacity: 1, y: 0 }}
            transition={{ duration: 0.5 }}
          >
            <Badge variant="outline" className="mb-6">
              LocalHost 2.4 is now generally available
            </Badge>
          </motion.div>
          <motion.h1
            initial={{ opacity: 0, y: 14 }}
            animate={{ opacity: 1, y: 0 }}
            transition={{ duration: 0.5, delay: 0.05 }}
            className="text-balance text-4xl font-semibold tracking-tight sm:text-6xl"
          >
            Enterprise virtualization. Built for performance.
          </motion.h1>
          <motion.p
            initial={{ opacity: 0, y: 14 }}
            animate={{ opacity: 1, y: 0 }}
            transition={{ duration: 0.5, delay: 0.1 }}
            className="mx-auto mt-6 max-w-2xl text-lg text-muted-foreground"
          >
            LocalHost is the virtualization platform organizations standardize on when reliability,
            security, and performance can't be negotiated — from a single workstation to a global fleet.
          </motion.p>
          <motion.div
            initial={{ opacity: 0, y: 14 }}
            animate={{ opacity: 1, y: 0 }}
            transition={{ duration: 0.5, delay: 0.15 }}
            className="mt-9 flex flex-wrap items-center justify-center gap-3"
          >
            <Button size="lg" asChild>
              <Link href="/download">
                Download LocalHost
                <ArrowRight className="h-4 w-4" />
              </Link>
            </Button>
            <Button size="lg" variant="outline" asChild>
              <Link href="/enterprise">
                <PlayCircle className="h-4 w-4" />
                Explore Enterprise
              </Link>
            </Button>
          </motion.div>
        </div>

        <motion.div
          initial={{ opacity: 0, y: 24 }}
          animate={{ opacity: 1, y: 0 }}
          transition={{ duration: 0.6, delay: 0.2 }}
          className="mx-auto mt-16 max-w-5xl"
        >
          <ScreenshotPlaceholder label="LocalHost Management Console" className="shadow-xl" />
        </motion.div>
      </div>
    </section>
  );
}

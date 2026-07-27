"use client";

import * as React from "react";
import { Mail } from "lucide-react";
import { Input } from "@/components/ui/input";
import { Button } from "@/components/ui/button";

export function Newsletter() {
  const [submitted, setSubmitted] = React.useState(false);

  return (
    <div className="rounded-xl border border-border bg-muted/30 p-8 sm:p-10">
      <div className="flex flex-col items-start justify-between gap-6 sm:flex-row sm:items-center">
        <div className="flex items-start gap-3">
          <div className="flex h-10 w-10 shrink-0 items-center justify-center rounded-md bg-primary/10 text-primary">
            <Mail className="h-5 w-5" />
          </div>
          <div>
            <h3 className="font-semibold">Engineering updates, once a month</h3>
            <p className="mt-1 text-sm text-muted-foreground">
              Release notes and platform engineering posts. No marketing noise.
            </p>
          </div>
        </div>
        {submitted ? (
          <p className="text-sm font-medium text-primary">You're subscribed. Thank you.</p>
        ) : (
          <form
            className="flex w-full max-w-sm shrink-0 gap-2"
            onSubmit={(e) => {
              e.preventDefault();
              setSubmitted(true);
            }}
          >
            <Input type="email" required placeholder="you@company.com" aria-label="Email address" />
            <Button type="submit">Subscribe</Button>
          </form>
        )}
      </div>
    </div>
  );
}

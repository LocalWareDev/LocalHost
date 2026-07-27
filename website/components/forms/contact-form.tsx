"use client";

import * as React from "react";
import { CheckCircle2 } from "lucide-react";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Textarea } from "@/components/ui/textarea";
import { Button } from "@/components/ui/button";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";

export function ContactForm() {
  const [submitted, setSubmitted] = React.useState(false);

  if (submitted) {
    return (
      <div className="flex items-start gap-3 rounded-lg border border-primary/30 bg-primary/5 p-6">
        <CheckCircle2 className="mt-0.5 h-5 w-5 shrink-0 text-primary" />
        <div>
          <p className="font-semibold">Message received</p>
          <p className="mt-1 text-sm text-muted-foreground">
            A member of the LocalWare team will respond within one business day.
          </p>
        </div>
      </div>
    );
  }

  return (
    <form
      className="space-y-5"
      onSubmit={(e) => {
        e.preventDefault();
        setSubmitted(true);
      }}
    >
      <div className="grid grid-cols-1 gap-5 sm:grid-cols-2">
        <div className="space-y-1.5">
          <Label htmlFor="name">Full name</Label>
          <Input id="name" name="name" required placeholder="Jane Cooper" />
        </div>
        <div className="space-y-1.5">
          <Label htmlFor="email">Work email</Label>
          <Input id="email" name="email" type="email" required placeholder="jane@company.com" />
        </div>
      </div>
      <div className="grid grid-cols-1 gap-5 sm:grid-cols-2">
        <div className="space-y-1.5">
          <Label htmlFor="company">Company</Label>
          <Input id="company" name="company" placeholder="Company name" />
        </div>
        <div className="space-y-1.5">
          <Label htmlFor="topic">Topic</Label>
          <Select name="topic" defaultValue="sales">
            <SelectTrigger id="topic">
              <SelectValue placeholder="Select a topic" />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="sales">Sales</SelectItem>
              <SelectItem value="support">Technical support</SelectItem>
              <SelectItem value="partnerships">Partnerships</SelectItem>
              <SelectItem value="press">Press</SelectItem>
              <SelectItem value="other">Other</SelectItem>
            </SelectContent>
          </Select>
        </div>
      </div>
      <div className="space-y-1.5">
        <Label htmlFor="message">Message</Label>
        <Textarea id="message" name="message" required placeholder="Tell us about your environment and what you're evaluating." />
      </div>
      <Button type="submit" size="lg">
        Send message
      </Button>
    </form>
  );
}

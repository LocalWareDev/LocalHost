"use client";

import * as React from "react";
import Link from "next/link";
import { Button } from "@/components/ui/button";

const STORAGE_KEY = "localhost-cookie-consent";

export function CookieConsent() {
  const [visible, setVisible] = React.useState(false);

  React.useEffect(() => {
    const consent = window.localStorage.getItem(STORAGE_KEY);
    if (!consent) setVisible(true);
  }, []);

  const respond = (value: "accepted" | "declined") => {
    window.localStorage.setItem(STORAGE_KEY, value);
    setVisible(false);
  };

  if (!visible) return null;

  return (
    <div className="fixed inset-x-0 bottom-0 z-50 border-t border-border bg-background/95 backdrop-blur">
      <div className="container flex flex-col items-start justify-between gap-4 py-4 md:flex-row md:items-center">
        <p className="text-sm text-muted-foreground">
          We use essential cookies to operate this site and optional analytics cookies to improve it. See our{" "}
          <Link href="/privacy" className="underline underline-offset-2">
            Privacy Policy
          </Link>{" "}
          for details.
        </p>
        <div className="flex shrink-0 gap-2">
          <Button variant="outline" size="sm" onClick={() => respond("declined")}>
            Decline
          </Button>
          <Button size="sm" onClick={() => respond("accepted")}>
            Accept
          </Button>
        </div>
      </div>
    </div>
  );
}

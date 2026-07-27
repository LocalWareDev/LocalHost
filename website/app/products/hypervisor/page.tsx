import type { Metadata } from "next";
import { notFound } from "next/navigation";
import { ProductPageTemplate } from "@/components/shared/product-page-template";
import { getProductBySlug } from "@/lib/data/products";

const product = getProductBySlug("hypervisor");

export const metadata: Metadata = {
  title: product?.name,
  description: product?.description,
};

export default function HypervisorPage() {
  if (!product) return notFound();
  return <ProductPageTemplate product={product} />;
}

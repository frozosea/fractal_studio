// Custom variant types — compile, list, and delete custom fractal formulas.

export interface CustomVariant {
  variantId: string;
  name: string;
  formula: string;
  bailout: number;
  bailoutSq?: number;
  createdAt: string;
  loaded: boolean;
}

export interface BuiltinVariantInfo {
  variantId: string;
  name: string;
  builtin: true;
}

export interface VariantListResponse {
  builtin: BuiltinVariantInfo[];
  custom: CustomVariant[];
}

export interface VariantCompileResponse {
  ok: boolean;
  variantId?: string;
  name?: string;
  hash?: string;
  bailout?: number;
  bailoutSq?: number;
  cached?: boolean;
  error?: string;
}

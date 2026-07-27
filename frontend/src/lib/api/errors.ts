export const ApiErrorCode = {
  NETWORK_ERROR: "NETWORK_ERROR",
  BAD_REQUEST: "BAD_REQUEST",
  CONFLICT: "CONFLICT",
  UNKNOWN: "UNKNOWN",
} as const;
export type ApiErrorCode = (typeof ApiErrorCode)[keyof typeof ApiErrorCode];

export class ApiError extends Error {
  public readonly code: ApiErrorCode;
  public readonly statusCode?: number;
  constructor(code: ApiErrorCode, message: string, options?: { statusCode?: number }) {
    super(message);
    this.name = "ApiError";
    this.code = code;
    this.statusCode = options?.statusCode;
  }
}

export function mapHttpError(status: number, body: Record<string, unknown>): ApiError {
  // Map HTTP status codes to ApiError codes
  const detail = typeof body.error === "string" ? body.error : String(body.error ?? "Unknown error");
  switch (status) {
    case 400: return new ApiError(ApiErrorCode.BAD_REQUEST, detail, { statusCode: status });
    case 409: return new ApiError(ApiErrorCode.CONFLICT, detail, { statusCode: status });
    default: return new ApiError(ApiErrorCode.UNKNOWN, detail || `Server error (${status})`, { statusCode: status });
  }
}

export interface CredentialsInput {
  email: string;
  password: string;
}

export interface UserView {
  id: string;
  email: string;
  roles: string[];
  status: "active" | "disabled";
  member?: boolean;
  creatorProfile?: { handle: string; displayName: string } | null;
}

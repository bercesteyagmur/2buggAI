import type { User } from "./types";

export function formatUser(user: User): string {
  return `${user.name} <${user.email}>`;
}

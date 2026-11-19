import { expect, test } from "vitest";
import { formatUser } from "./format";

test("formatUser shows the full name and the email", () => {
  const user = { id: 1, full_name: "Ada Lovelace", email: "ada@example.com" };
  expect(formatUser(user)).toBe("Ada Lovelace <ada@example.com>");
});

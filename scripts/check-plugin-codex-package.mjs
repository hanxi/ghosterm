import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";
import { createHash } from "node:crypto";

const [tools, directory] = process.argv.slice(2);
const require = createRequire(path.join(tools, "package.json"));
const Ajv = require("ajv/dist/2020");
const ajv = new Ajv({ strict: false });
const load = p => JSON.parse(fs.readFileSync(p, "utf8"));
const schemaRoot = new URL("../packages/plugin-sdk/schema/", import.meta.url);
ajv.addSchema(load(new URL("bridge-v1.candidate.json", schemaRoot)));
for (const [file, schema] of [["plugin.json", "manifest-v1.candidate.json"], ["integrity.json", "integrity-v1.candidate.json"]]) {
  const validate = ajv.compile(load(new URL(schema, schemaRoot)));
  assert(validate(load(path.join(directory, file))), JSON.stringify(validate.errors));
}
const manifest = load(path.join(directory, "plugin.json"));
assert.equal(manifest.id, "org.ghosterm.ai-codex");
assert.deepEqual(manifest.commands[0].path, ["ai", "codex"]);
assert.deepEqual(manifest.client.requires, ["session.rpc"]);
assert.deepEqual(manifest.permissions.find(p => p.capability === "process.spawn").programs, ["codex"]);
assert(!manifest.permissions.some(p => p.side === "client" && p.capability.startsWith("process.")));
for (const member of load(path.join(directory, "integrity.json")).members) {
  const bytes = fs.readFileSync(path.join(directory, member.path));
  assert.equal(bytes.length, member.bytes);
  assert.equal(createHash("sha256").update(bytes).digest("hex"), member.sha256);
}
const argumentSchema = load(path.join(directory, "schemas/command.json"));
assert.deepEqual(argumentSchema.required, [], "Native CLI argument schema requires an explicit required list");
const args = ajv.compile(argumentSchema);
assert(args({}));
assert(!args({ path: "/unexpected" }));
const { activate } = await import(pathToFileURL(path.join(directory, "client/index.js")));
const client = activate({ side: "client", invocation: { command: "ai codex" } });
client.dispose();
assert.throws(() => activate({ side: "executor", invocation: { command: "ai codex" } }));
console.log("CODEX PACKAGE METADATA PASS: schemas, integrity, process scope, args, compiled Client");

import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = await readFile(new URL("../plugins/official/preview/executor/index.js", import.meta.url), "utf8");
const { activate } = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);

async function run(args) {
  const publications = [];
  const context = {
    side: "executor",
    invocation: { command: "preview", decodedArguments: args },
    resources: {
      workspaces: ["workspace"],
      publish(input, limits) {
        publications.push({ input, limits });
        return { outcome: Promise.resolve({ kind: "success", value: { id: String(publications.length) } }) };
      },
    },
  };
  await activate(context);
  return publications;
}

const publications = await run({
  path: "site/index.html",
  assets: "site/theme.css,site/images/logo.png",
  maxBytes: 65536,
});
assert.deepEqual(publications.map(({ input }) => [input.relativePath, input.mediaType]), [
  ["site/index.html", "text/html"],
  ["site/theme.css", "text/css"],
  ["site/images/logo.png", "image/png"],
]);
assert.ok(publications.every(({ input }) => input.root === "workspace"));
assert.ok(publications.every(({ limits }) => limits.byteLimit === "65536"));

const commaAsset = await run({
  path: "site/index.md",
  assets: "site/theme\\,dark.css",
  maxBytes: 65536,
});
assert.equal(commaAsset[1].input.relativePath, "site/theme,dark.css",
  "a comma in one canonical filename must not become two attachment paths");

const unicodePathAtLimit = `${Array(255).fill("😀").join("/")}/ab`;
assert.equal([...unicodePathAtLimit].length, 512);
const unicodePathPublications = await run({
  path: unicodePathAtLimit,
  assets: "",
  maxBytes: 65536,
});
assert.equal(unicodePathPublications[0].input.relativePath, unicodePathAtLimit,
  "the Executor path bound must use Unicode code points like argsSchema.maxLength");
const unicodePathOverLimit = `${Array(255).fill("😀").join("/")}/abc`;
assert.equal([...unicodePathOverLimit].length, 513);
await assert.rejects(() => run({ path: unicodePathOverLimit, assets: "", maxBytes: 65536 }),
  /canonical relative path/);

for (const assets of ["../secret.txt", "/etc/passwd", "site/a.css,,site/b.png", "site/index.html", "site/a.css,site/a.css"]) {
  const invalid = {
    side: "executor",
    invocation: { command: "preview", decodedArguments: { path: "site/index.html", assets, maxBytes: 65536 } },
    resources: { workspaces: ["workspace"], publish() { assert.fail("invalid assets must be rejected before publishing"); } },
  };
  await assert.rejects(() => activate(invalid), /relative attachment paths/);
}

for (const assets of ["site/a\\x.png", "site/a\\"]) {
  await assert.rejects(() => run({ path: "site/index.md", assets, maxBytes: 65536 }),
    /invalid escape/);
}

const nonCanonicalMain = {
  side: "executor",
  invocation: { command: "preview", decodedArguments: { path: "site/../index.html", assets: "", maxBytes: 65536 } },
  resources: { workspaces: ["workspace"], publish() { assert.fail("invalid main path must be rejected before publishing"); } },
};
await assert.rejects(() => activate(nonCanonicalMain), /canonical relative path/);

const tooMany = {
  side: "executor",
  invocation: { command: "preview", decodedArguments: {
    path: "site/index.html", assets: Array.from({ length: 7 }, (_, i) => `site/${i}.png`).join(","), maxBytes: 65536,
  } },
  resources: { workspaces: ["workspace"], publish() { assert.fail("too many assets must be rejected before publishing"); } },
};
await assert.rejects(() => activate(tooMany), /at most 6/);

const wrongType = {
  side: "executor",
  invocation: { command: "preview", decodedArguments: {
    path: "site/index.html", assets: "site/run.js", maxBytes: 65536,
  } },
  resources: {
    workspaces: ["workspace"],
    publish(input) {
      assert.equal(input.relativePath, "site/index.html");
      return { outcome: Promise.resolve({ kind: "success", value: { id: "main" } }) };
    },
  },
};
await assert.rejects(() => activate(wrongType), /attachment type is not allowed/);

console.log("plugin preview assets: PASS");

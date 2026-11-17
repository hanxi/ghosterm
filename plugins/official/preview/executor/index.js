const MIME_BY_EXTENSION = Object.freeze({
  txt: "text/plain",
  log: "text/plain",
  md: "text/markdown",
  markdown: "text/markdown",
  html: "text/html",
  htm: "text/html",
  css: "text/css",
  png: "image/png",
  jpg: "image/jpeg",
  jpeg: "image/jpeg",
  gif: "image/gif",
  webp: "image/webp",
});
const MAX_ASSETS = 6;

function mediaType(path) {
  const leaf = path.slice(path.lastIndexOf("/") + 1);
  const dot = leaf.lastIndexOf(".");
  const extension = dot < 0 ? "" : leaf.slice(dot + 1).toLowerCase();
  return MIME_BY_EXTENSION[extension] || "application/octet-stream";
}

function relativePath(path) {
  return typeof path === "string" && path.length > 0 && [...path].length <= 512 &&
    !path.startsWith("/") && !path.includes("\\") && !path.includes("\0") &&
    !path.split("/").some(part => !part || part === "..") &&
    path !== "." && !path.endsWith("/.");
}

// Remove only identity components. Do not collapse `a/../b`: that requires
// opening a in the actual filesystem and must never bypass a scoped ancestor.
function canonicalPath(path) {
  return path.split("/").filter(part => part !== ".").join("/");
}

function attachmentPaths(value, mainPath) {
  if (!relativePath(mainPath)) throw new Error("preview path must be a canonical relative path of at most 512 characters");
  if (value === "") return [];
  if (typeof value !== "string") throw new Error("preview assets must be a comma-separated path list");
  const paths = [];
  let path = "";
  for (let index = 0; index < value.length; index++) {
    const character = value[index];
    if (character === "\\") {
      if (value[index + 1] !== ",") {
        throw new Error("preview asset list has an invalid escape before a non-comma character");
      }
      path += ",";
      index++;
    } else if (character === ",") {
      paths.push(path);
      path = "";
    } else {
      path += character;
    }
  }
  paths.push(path);
  if (paths.length > MAX_ASSETS || paths.some(path => !relativePath(path))) {
    throw new Error(`preview accepts at most ${MAX_ASSETS} unique relative attachment paths`);
  }
  const canonical = paths.map(canonicalPath);
  if (new Set(canonical).size !== canonical.length || canonical.includes(mainPath)) {
    throw new Error(`preview accepts at most ${MAX_ASSETS} unique relative attachment paths`);
  }
  return canonical;
}

async function publish(context, path, mime, maxBytes) {
  const transfer = context.resources.publish(
    { root: "workspace", relativePath: path, mediaType: mime },
    { byteLimit: String(maxBytes), timeoutMs: 30000 },
  );
  const outcome = await transfer.outcome;
  if (outcome.kind !== "success") {
    throw new Error(`preview resource publication for ${path} ${outcome.kind}`);
  }
  return outcome.value;
}

export async function activate(context) {
  if (context.side !== "executor" || context.invocation?.command !== "preview") {
    throw new Error("preview requires the preview command on the Executor side");
  }
  const args = context.invocation.decodedArguments;
  if (!args || typeof args.path !== "string" || typeof args.maxBytes !== "number") {
    throw new Error("preview arguments were not validated by the host");
  }
  if (!context.resources || !context.resources.workspaces.includes("workspace")) {
    throw new Error("preview requires the explicitly selected workspace root");
  }
  if (!relativePath(args.path)) throw new Error("preview path must resolve to a canonical relative path of at most 512 characters");
  const mainPath = canonicalPath(args.path);
  const attachments = attachmentPaths(args.assets, mainPath);
  const mainType = mediaType(mainPath);
  if (attachments.length && mainType !== "text/html" && mainType !== "text/markdown") {
    throw new Error("preview attachments are supported only for HTML and Markdown documents");
  }
  const attachmentTypes = attachments.map((path) => {
    const type = mediaType(path);
    if (type !== "text/css" && !type.startsWith("image/")) {
      throw new Error(`preview attachment type is not allowed: ${path}`);
    }
    return type;
  });
  await publish(context, mainPath, mainType, args.maxBytes);
  for (let index = 0; index < attachments.length; index++) {
    await publish(context, attachments[index], attachmentTypes[index], args.maxBytes);
  }
  return { dispose() {} };
}

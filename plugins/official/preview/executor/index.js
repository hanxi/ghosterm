const MIME_BY_EXTENSION = Object.freeze({
  txt: "text/plain",
  log: "text/plain",
  md: "text/markdown",
  markdown: "text/markdown",
  html: "text/html",
  htm: "text/html",
  png: "image/png",
  jpg: "image/jpeg",
  jpeg: "image/jpeg",
  gif: "image/gif",
  webp: "image/webp",
});

function mediaType(path) {
  const leaf = path.slice(path.lastIndexOf("/") + 1);
  const dot = leaf.lastIndexOf(".");
  const extension = dot < 0 ? "" : leaf.slice(dot + 1).toLowerCase();
  return MIME_BY_EXTENSION[extension] || "application/octet-stream";
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
  const transfer = context.resources.publish(
    { root: "workspace", relativePath: args.path, mediaType: mediaType(args.path) },
    { byteLimit: String(args.maxBytes), timeoutMs: 30000 },
  );
  const outcome = await transfer.outcome;
  if (outcome.kind !== "success") {
    throw new Error(`preview resource publication ${outcome.kind}`);
  }
  return { dispose() {} };
}

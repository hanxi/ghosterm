export function activate(context) {
  if (context.side !== "client") throw new Error("preview Client loaded on the wrong side");
  return { dispose() {} };
}

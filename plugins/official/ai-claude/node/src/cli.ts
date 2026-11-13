// Keep dependency loading inside the guarded entry so a missing SDK is an
// explicit unavailable exit, with no installation or system-CLI fallback.
async function main(): Promise<number> {
  if (Number(process.versions.node.split('.')[0]) < 20) {
    process.stderr.write("claude-node: NODE_VERSION_UNSUPPORTED\n"); return 4;
  }
  if (process.argv.length !== 4 || process.argv[2] !== "--profile") {
    process.stderr.write("claude-node: expected --profile ABSOLUTE_PROFILE_FILE\n"); return 2;
  }
  try {
    const { runClaudeExecutor } = await import("./executor.js");
    return await runClaudeExecutor(process.argv[3]!, process.stdin, process.stdout);
  } catch (error) {
    const unavailable = typeof error === "object" && error !== null && "code" in error && error.code === "ERR_MODULE_NOT_FOUND";
    process.stderr.write(unavailable ? "claude-node: DEPENDENCY_UNAVAILABLE\n" : "claude-node: PROFILE_OR_RUNTIME_ERROR\n");
    return unavailable ? 4 : 2;
  }
}
process.exitCode = await main();

import { object, text } from "@ghosterm/plugin-sdk";
import type { PluginContext, ProcessChild } from "@ghosterm/plugin-sdk";
const Echo = { name: "dev.echo", input: object({ text: text(64) }), output: object({ text: text(64) }) };
const Exchange = { ...Echo, name: "dev.exchange" };
const Callback = { ...Echo, name: "dev.callback" };
export function activate(context: PluginContext) {
  const process = context.process, cwd = context.invocation?.workspaceHandle;
  if (!process || !cwd || cwd.id !== "workspace") throw new Error("Explicit process service required");
  let wake: (() => void) | undefined;
  const tick = () => new Promise<void>(resolve => { wake = resolve; });
  async function business(mode: string): Promise<string> {
    if (mode === "business-wait") mode = "wait";
    if (mode === "throw") throw new Error("Declared error");
    const program = mode === "missing" ? "missing" : "business";
    const directory = mode === "outside" ? { id: "outside" } : cwd;
    if (!directory || !process) throw new Error("Revoked");
    let child: ProcessChild;
    try {
      child = await process.spawn({ program, argv: ["business", "quoted ' \" $() 中文", mode === "stderr" ? "stderr" : mode === "cancel" || mode === "wait" ? "wait" : "roundtrip"], cwd: directory, envPolicy: "selected-user-environment", cancellation: context.cancellation });
    } catch (error) { if (mode !== "missing" && mode !== "outside") throw error; return "DENIED"; }
    try {
      if (child.verifiedVersion !== "1.0.0") throw new Error("Version");
      if (mode === "wait") return await new Promise<string>(() => {});
      const input = new Uint8Array(mode === "stderr" || mode === "cancel" ? 0 : 2048);
      for (let i = 0; i < input.length; i++) input[i] = i % 251;
      let sent = 0, received = 0, errors = "", closed = false, eof = false, errorEOF = false, signalled = false;
      for (;;) {
        if (sent < input.length) {
          const result = child.writeStdin(input.subarray(sent));
          if (result.state === "data") sent += result.count;
          else if (result.state !== "wait") throw new Error("stdin");
        }
        if (!closed && sent === input.length) { child.closeStdin(); closed = true; }
        if (!eof) {
          const result = child.readStdout(1024);
          if (result.state === "data") {
            for (let i = 0; i < result.bytes.length; i++) {
              if (mode !== "cancel" && result.bytes[i] !== (received + i) % 251) throw new Error("stdout byte");
            }
            received += result.bytes.length;
          } else if (result.state === "eof") eof = true;
          else if (result.state !== "wait") throw new Error("stdout");
        }
        if (!errorEOF) {
          const result = child.readStderr(1024);
          if (result.state === "data") errors += String.fromCharCode(...result.bytes);
          else if (result.state === "eof") errorEOF = true;
          else if (result.state !== "wait") throw new Error("stderr");
        }
        if (mode === "cancel" && received === 5 && !signalled) { child.signal("terminate"); signalled = true; }
        const state = child.poll();
        if (state.state === "exited" && eof && errorEOF) {
          if (state.terminationReason) throw new Error("Unexpected budget termination");
          if (mode === "cancel") {
            if (state.signal !== 15 || errors) throw new Error("termination");
            return "process:signal:15";
          }
          if (state.signal !== null || state.code !== (mode === "stderr" ? 7 : 0) || received !== input.length || errors !== (mode === "stderr" ? "stderr-proof" : "")) throw new Error("exit/streams");
          return "process:" + received + ":" + state.code;
        }
        await tick();
      }
    } finally {
      if (child.poll().state === "running") child.signal("terminate");
      await child.release();
    }
  }
  context.session.register(Echo, async input => ({ text: await business(input.text) }));
  context.session.register(Exchange, async input => {
    const result = await business(input.text);
    const reply = await context.session.request(Callback, { text: result }, 1000).outcome;
    if (reply.kind !== "success") throw new Error("Callback");
    return { text: "executor:" + reply.value.text };
  });
  return { step() { const next = wake; wake = undefined; next?.(); }, dispose() { wake?.(); } };
}

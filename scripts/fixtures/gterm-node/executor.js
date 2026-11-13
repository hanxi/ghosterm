/* Explicit no-model conformance fixture, not an official Claude plugin. */
import { object, text, integer } from "@ghosterm/plugin-sdk";

export async function activate(ctx) {
  const child = await ctx.process.spawn({ program: "cat", argv: ["cat"],
    cwd: ctx.invocation.workspaceHandle, envPolicy: "selected-user-environment",
    cancellation: ctx.cancellation });
  let sequence = 0, outstanding, outgoing = [], offset = 0, incoming = "";
  let stopping = false, releasing = false, released = false, failure;
  let resolve, reject;
  const finished = new Promise((yes, no) => { resolve = yes; reject = no; });
  void finished.catch(() => {});
  function fail(error) { if (!failure) { failure = error; reject(error); } }
  function send(method) {
    outstanding = { method, id: `gterm-${++sequence}`, sequence };
    const line = JSON.stringify({ id: outstanding.id, method, params: {}, sequence, version: 1 }) + "\n";
    outgoing = Array.from(line, value => value.charCodeAt(0)); offset = 0;
  }
  const codec = object({ text: text(40), count: integer(0, 100) });
  ctx.session.register({ name: "fixture.echo", input: codec, output: codec }, async (value, request) => {
    request.check(); await finished; request.check(); return value;
  });
  send("start");
  return {
    step() {
      if (failure || released) return;
      try {
        if (offset < outgoing.length) {
          const write = child.writeStdin(new Uint8Array(outgoing.slice(offset)));
          if (write.state === "data") offset += write.count;
          else if (write.state !== "wait") throw Error("Node fixture write failed");
        }
        const read = child.readStdout(16384);
        if (read.state === "data") for (const byte of read.bytes) {
          if (byte >= 128) throw Error("fixture control response is not ASCII");
          if (byte !== 10) {
            if (incoming.length >= 3072) throw Error("fixture control response too large");
            incoming += String.fromCharCode(byte); continue;
          }
          const response = JSON.parse(incoming); incoming = "";
          if (!outstanding || response.version !== 1 || response.id !== outstanding.id ||
              response.sequence !== outstanding.sequence || response.ok !== true) throw Error("uncorrelated Node fixture response");
          if (outstanding.method === "stop") {
            if (response.result.phase !== "closed") throw Error("Node did not accept stop");
            stopping = true; outstanding = undefined;
          } else if (outstanding.method === "start" || response.result.phase !== "ready") send("poll");
          else send("stop");
        }
        else if (read.state !== "wait" && !(read.state === "eof" && stopping)) throw Error("Node fixture stdout ended early");
        const errors = child.readStderr(16384);
        if (errors.state === "data" && errors.bytes.length) throw Error("Node fixture reported an error");
        const exit = child.poll();
        if (exit.state === "exited" && !releasing) {
          if (!stopping || exit.code !== 0 || exit.signal !== null) throw Error("Node fixture exit was not confirmed");
          releasing = true;
          child.release().then(() => { released = true; resolve(); }).catch(fail);
        }
      } catch (error) { fail(error); }
    },
    dispose() { if (!released) child.signal("terminate"); },
  };
}

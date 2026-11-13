// Named in-memory CLI control fixture behind the REAL locked Agent SDK.
// No model, shell, tool execution or real CLI version evidence is claimed here.
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { PassThrough, Writable } from "node:stream";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
const config = JSON.parse(readFileSync(process.argv[3], "utf8"));
const component = resolve(dirname(config.executable), "../../..");
const { ClaudeApprovalBroker, ClaudeNodeSession, ClaudeStdioBridge, serveClaudeStdio } = await import(pathToFileURL(join(component, "dist/index.js")));
const command = "fixture only 中文😀\n".repeat(1000);
const questions = Array.from({ length: 4 }, (_, i) => ({ question: `question-${i}-${"问😀".repeat(80)}`, header: `Q${i}`,
  multiSelect: false, options: [{ label: "One", description: "First" }, { label: "Two", description: "Second" }] }));
class Fixture extends EventEmitter {
  stdout = new PassThrough(); killed = false; exitCode = null; signalCode = null; input = ""; uuid; userText; approvals = 0;
  stdin = new Writable({ write: (chunk, _, done) => { try { this.receive(chunk.toString()); done(); } catch (error) { done(error); } } });
  frame(value) { this.stdout.write(JSON.stringify(value) + "\n"); }
  receive(chunk) {
    this.input += chunk;
    for (;;) {
      const at = this.input.indexOf("\n"); if (at < 0) return;
      const value = JSON.parse(this.input.slice(0, at)); this.input = this.input.slice(at + 1);
      if (value.type === "control_request" && value.request.subtype === "initialize") {
        this.frame({ type: "control_response", response: { subtype: "success", request_id: value.request_id,
          response: { commands: [], agents: [], models: [], account: {}, output_style: "default", available_output_styles: [] } } });
      } else if (value.type === "user") {
        this.uuid = value.uuid; this.userText = value.message.content;
        this.frame({ type: "control_request", request_id: "fixture-审批😀", request: { subtype: "can_use_tool", tool_name: "Bash",
          input: { command }, tool_use_id: "fixture-tool" } });
      } else if (value.type === "control_response" && value.response.request_id === "fixture-审批😀") {
        const reply = value.response.response; this.approvals++;
        assert.equal(reply.behavior, "allow"); assert.equal(reply.updatedInput.command, command); assert.equal(reply.updatedPermissions, undefined);
        this.frame({ type: "control_request", request_id: "fixture-question😀", request: { subtype: "can_use_tool",
          tool_name: "AskUserQuestion", input: { questions }, tool_use_id: "fixture-question-tool" } });
      } else if (value.type === "control_response" && value.response.request_id === "fixture-question😀") {
        const reply = value.response.response;
        assert.equal(reply.behavior, "allow"); assert.equal(reply.updatedPermissions, undefined);
        assert.deepEqual(reply.updatedInput.questions, questions);
        this.frame({ type: "system", subtype: "fixture-audit", approvals: this.approvals, originalRequestID: "fixture-审批😀",
          answers: reply.updatedInput.answers, userText: this.userText, command });
        this.frame({ type: "result", subtype: "success", is_error: false, user_message_uuid: this.uuid, uuid: "fixture-result", session_id: "fixture-session" });
      }
    }
  }
  kill(signal = "SIGTERM") {
    if (this.killed) return false;
    this.killed = true; this.signalCode = signal; this.stdout.end(); queueMicrotask(() => this.emit("exit", null, signal)); return true;
  }
}
const child = new Fixture(), broker = new ClaudeApprovalBroker("native-sdk-fixture");
const session = new ClaudeNodeSession({ cwd: config.cwd, executable: config.executable, cliVersion: config.cliVersion,
  environment: Object.fromEntries(config.environmentNames.map(name => [name, process.env[name]])), tools: ["Bash", "AskUserQuestion"],
  approvals: broker, abortController: new AbortController(), spawn: () => child });
const bridge = new ClaudeStdioBridge(session, broker);
try { process.exitCode = await serveClaudeStdio(process.stdin, process.stdout, bridge) === "stopped" ? 0 : 7; }
finally {
  bridge.close();
  const end = Date.now() + 5000;
  while (!child.killed && Date.now() < end) await new Promise(resolve => setTimeout(resolve, 5));
  assert(child.killed || session.phase === "new", "fixture SDK child must close");
}

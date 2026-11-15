/** Genuine bounded sieve work. Each pump performs at most 512 primitive units;
 * cancellation is acknowledged separately and observed at the next pump.
 * Disposing memory is not a confirmed cancellation result. */
export class PrimeTask {
  private composite: Uint8Array | undefined;
  private candidate = 1;
  private multiple = 1;
  private count = 0;
  private completed = 0;
  private sequence = 1n;
  private phase: "queued" | "running" | "cancelling" | "succeeded" | "cancelled" = "queued";
  private live = true;
  constructor(readonly limit: number) {
    if (!Number.isSafeInteger(limit) || limit < 0 || limit > 1000000) throw new Error("Task limit must be 0..1000000");
  }
  private check(): void { if (!this.live) throw new Error("Task has been disposed"); }
  cancel(): "requested" | "alreadyFinished" {
    this.check();
    if (this.phase === "succeeded" || this.phase === "cancelled") return "alreadyFinished";
    if (this.phase !== "cancelling") { this.phase = "cancelling"; this.sequence++; }
    return "requested";
  }
  step(): void {
    this.check();
    if (this.phase === "succeeded" || this.phase === "cancelled") return;
    if (this.phase === "cancelling") { this.phase = "cancelled"; this.sequence++; return; }
    this.composite ??= new Uint8Array(this.limit + 1);
    this.phase = "running";
    let remaining = 512;
    while (remaining-- > 0) {
      if (this.multiple <= this.limit && this.candidate >= 2) {
        this.composite![this.multiple] = 1; this.multiple += this.candidate;
      } else {
        this.candidate++;
        if (this.candidate > this.limit) { this.phase = "succeeded"; this.composite = undefined; break; }
        this.completed++;
        if (this.composite![this.candidate] === 0) { this.count++; this.multiple = this.candidate * this.candidate; }
        else this.multiple = this.limit + 1;
      }
    }
    this.sequence++;
  }
  snapshot() {
    this.check();
    return Object.freeze({ sequence: this.sequence.toString(), phase: this.phase, completed: String(this.completed),
      total: Object.freeze({ kind: "known" as const, units: String(Math.max(0, this.limit - 1)) }),
      message: this.phase === "succeeded" ? "Prime counting complete" : this.phase === "cancelled" ? "Prime counting cancelled" :
        this.phase === "cancelling" ? "Cancellation requested; waiting for worker" : "Count primes using a bounded sieve",
      result: this.phase === "succeeded" ? Object.freeze({ kind: "success" as const, summary: `${this.count} primes up to ${this.limit}` }) :
        this.phase === "cancelled" ? Object.freeze({ kind: "cancelled" as const, effect: "none" as const }) : Object.freeze({ kind: "none" as const }) });
  }
  dispose(): void { this.live = false; this.composite = undefined; }
}

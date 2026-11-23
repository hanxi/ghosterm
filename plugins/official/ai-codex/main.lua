local manifest = {
  id = "org.ghosterm.ai-codex",
  version = "0.1.0",
  api = "1",
  publisher = "Ghosterm",
  license = "MIT",
  client = {
    requires = { "session.rpc" },
  },
  executor = {
    rpc = {
      "codex.thread.start",
      "codex.turn.send",
      "codex.turn.interrupt",
      "codex.approval.respond",
      "codex.change.read",
    },
    entry = "executor.lua",
    requires = { "session.rpc", "process.spawn", "process.pipes" },
    programs = { "codex" },
  },
  files = { "protocol/app-server.lock.json" },
  commands = {
    {
      path = { "ai", "codex" },
      title = "Codex conversation",
      argsSchema = "schemas/command.json",
      views = { "chat", "approval" },
    },
  },
}

return {
  ["ai codex"] = function()
    return true
  end,
}

local manifest = {
  id = "org.ghosterm.ai-claude",
  version = "0.1.0",
  api = "1",
  publisher = "Ghosterm",
  license = "MIT",
  client = {
    requires = { "session.rpc" },
  },
  executor = {
    rpc = {
      "claude.input.open",
      "claude.input.append",
      "claude.input.seal",
      "claude.input.discard",
      "claude.call",
      "claude.response.read",
      "claude.response.forget",
      "claude.status",
    },
    entry = "executor.lua",
    requires = { "session.rpc", "process.spawn", "process.pipes" },
    programs = { "claude" },
  },
  commands = {
    { path = { "ai", "claude" }, title = "Claude conversation", argsSchema = "schemas/command.json" },
  },
}

return {
  ["ai claude"] = function()
    return true
  end,
}

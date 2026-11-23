local manifest = {
  id = "org.ghosterm.task",
  version = "0.1.0",
  api = "1",
  publisher = "Ghosterm",
  license = "MIT",
  client = {
    requires = { "session.rpc", "ui.task" },
  },
  executor = {
    rpc = {
      "view.task.describe",
      "view.task.read",
      "view.task.cancel",
    },
    entry = "executor.lua",
    requires = { "session.rpc" },
  },
  commands = {
    {
      path = { "task", "primes" },
      title = "Count primes as a cancellable task",
      argsSchema = "schemas/primes.json",
      views = { "task" },
    },
  },
}

return {
  ["task primes"] = function()
    return true
  end,
}

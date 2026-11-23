local manifest = {
  id = "org.ghosterm.logs",
  version = "0.1.0",
  api = "1",
  publisher = "Ghosterm",
  license = "MIT",
  client = {
    requires = { "session.rpc", "ui.table" },
  },
  executor = {
    rpc = {
      "view.table.describe",
      "view.table.page",
      "view.table.search",
      "logs.page",
    },
    entry = "executor.lua",
    requires = { "session.rpc" },
  },
  commands = {
    {
      path = { "logs" },
      title = "Sample logs",
      argsSchema = "schemas/logs.json",
      views = { "table" },
    },
  },
}

return {
  logs = function()
    return true
  end,
}

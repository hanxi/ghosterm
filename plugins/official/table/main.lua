local manifest = {
  id = "org.ghosterm.table",
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
    },
    entry = "executor.lua",
    requires = { "session.rpc" },
  },
  commands = {
    {
      path = { "table" },
      title = "Paged number powers",
      argsSchema = "schemas/table.json",
      views = { "table" },
      togglePanel = true,
    },
  },
}

return {
  table = function()
    return true
  end,
}

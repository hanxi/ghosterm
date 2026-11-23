local manifest = {
  id = "org.ghosterm.diff",
  version = "0.1.0",
  api = "1",
  publisher = "Ghosterm",
  license = "MIT",
  client = {
    requires = { "session.rpc", "ui.diff" },
  },
  executor = {
    rpc = {
      "view.diff.describe",
      "view.diff.page",
    },
    entry = "executor.lua",
    requires = { "session.rpc" },
  },
  commands = {
    {
      path = { "diff" },
      title = "Read-only text comparison",
      argsSchema = "schemas/diff.json",
      views = { "diff" },
    },
  },
}

return {
  diff = function()
    return true
  end,
}

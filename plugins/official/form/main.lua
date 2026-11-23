local manifest = {
  id = "org.ghosterm.form",
  version = "0.1.0",
  api = "1",
  publisher = "Ghosterm",
  license = "MIT",
  client = {
    requires = { "session.rpc", "ui.form" },
  },
  executor = {
    rpc = {
      "form.status",
      "form.cancel",
    },
    entry = "executor.lua",
    requires = { "session.rpc" },
  },
  commands = {
    {
      path = { "form" },
      title = "Try a plugin form",
      argsSchema = "schemas/form.json",
    },
  },
}

return {
  form = function()
    return true
  end,
}

local manifest = {
  id = "org.ghosterm.signature",
  version = "1.0.0",
  api = "1",
  publisher = "fixture",
  license = "MIT",
  client = { requires = { "session.rpc" } },
  commands = { { path = { "fixture" }, title = "Signature fixture" } },
}

return {
  fixture = function()
    return true
  end,
}

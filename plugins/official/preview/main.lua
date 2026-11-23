local manifest = {
  id = "org.ghosterm.preview",
  version = "0.1.0",
  api = "1",
  publisher = "Ghosterm",
  license = "MIT",
  files = { "LICENSES/MIT.txt" },
  client = {
    requires = { "session.rpc", "resource.receive" },
  },
  executor = {
    rpc = {
      "preview.open",
    },
    entry = "executor.lua",
    requires = { "session.rpc", "resource.receive", "file.readScoped" },
  },
  commands = {
    {
      path = { "preview" },
      title = "Preview a scoped file",
      argsSchema = "schemas/preview.json",
      views = { "resource" },
    },
  },
}

local function preview(ctx, args)
  if args.path == nil and #args == 1 then
    args = { path = args[1], assets = "", maxBytes = 262144 }
  end
  return ctx.rpc.call("preview.open", args, 300000)
end

return {
  preview = preview,
}

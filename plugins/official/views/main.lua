local manifest = {
  id = "org.ghosterm.views",
  version = "0.1.0",
  api = "1",
  publisher = "Ghosterm",
  license = "MIT",
  client = {
    rpc = { "views.client.open" },
    requires = { "session.rpc", "ui.table", "ui.diff", "ui.task", "ui.views" },
  },
  executor = {
    rpc = {
      "alpha.describe",
      "alpha.page",
      "beta.describe",
      "beta.page",
      "changes.describe",
      "changes.page",
      "work.describe",
      "work.read",
      "work.cancel",
      "views.reopen",
    },
    entry = "executor.lua",
    requires = { "session.rpc" },
  },
  commands = {
    {
      path = { "views", "demo" },
      title = "Two tables, a diff and a task",
      argsSchema = "schemas/demo.json",
      views = { "table", "diff", "task" },
    },
  },
}

local ctx = ...
if not ctx.views then
  return nil
end

local specs = {
  alpha = { "table", "alpha.describe", "alpha.page" },
  beta = { "table", "beta.describe", "beta.page" },
  changes = { "diff", "changes.describe", "changes.page" },
  progress = { "task", "work.describe", "work.read", "work.cancel" },
}
local handles = {}

local function open(id)
  local spec = specs[id]
  if not spec then
    return nil, { code = "INVALID_INPUT", message = "Unknown view", effect = "none" }
  end
  if handles[id] and ctx.views.isOpen(handles[id]) then
    return { opened = false }
  end
  local handle = ctx.views.open(spec[1], id, spec[2], spec[3], spec[4])
  if not handle then
    return nil, { code = "DENIED", message = "Cannot open view", effect = "none" }
  end
  handles[id] = handle
  return { opened = true }
end

local function demo()
  local order = { "alpha", "beta", "changes", "progress" }
  for i = 1, #order do
    local result, err = open(order[i])
    if not result then
      return nil, err
    end
  end
  return true
end

return {
  ["views demo"] = demo,
  ["views.client.open"] = function(input)
    if type(input) ~= "table" or type(input.viewId) ~= "string" then
      return nil, { code = "INVALID_INPUT", message = "Missing view ID", effect = "none" }
    end
    return open(input.viewId)
  end,
}

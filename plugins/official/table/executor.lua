local ctx = ...
local total = ctx.arguments and ctx.arguments.count
local page_size = 16
local project = ctx.arguments and ctx.arguments.project or ""

if math.type(total) ~= "integer" or total < 0 or total > 1000000 then
  return nil
end

local projects = {
  ["sample-a"] = {
    { id = "sample-a-review", cells = { "Review release notes", "In progress" } },
    { id = "sample-a-publish", cells = { "Publish release", "Open" } },
  },
  ["sample-b"] = {
    { id = "sample-b-inventory", cells = { "Update inventory", "Done" } },
    { id = "sample-b-verify", cells = { "Verify stock count", "Open" } },
  },
}
local tasks = projects[project]
if project ~= "" and tasks == nil then
  return nil
end

local description = {
  viewId = ctx.instanceId,
  revision = ctx.epoch,
  title = "Number powers",
  columns = {
    { id = "n", title = "Number" },
    { id = "square", title = "Square" },
    { id = "cube", title = "Cube" },
  },
  total = tasks and #tasks or total,
  pageSize = page_size,
}

if tasks then
  description.title = "Project tasks · " .. project
  description.columns = {
    { id = "task", title = "Task" },
    { id = "status", title = "Status" },
  }
end

local function describe()
  return description
end

local function page(input)
  if type(input) ~= "table" or input.viewId ~= description.viewId
      or input.revision ~= description.revision or math.type(input.offset) ~= "integer"
      or math.type(input.limit) ~= "integer" or input.offset < 0 or input.offset > description.total
      or input.limit < 1 or input.limit > page_size then
    return nil, { code = "INVALID_INPUT", message = "Stale or invalid table page", effect = "none" }
  end

  local rows = ctx.json.array()
  for i = 1, math.min(input.limit, description.total - input.offset) do
    local n = input.offset + i
    if tasks then
      rows[i] = tasks[n]
    else
      local label = string.format("%d", n)
      rows[i] = {
        id = label,
        cells = { label, string.format("%d", n * n), string.format("%d", n * n * n) },
      }
    end
  end
  return {
    viewId = description.viewId,
    revision = description.revision,
    offset = input.offset,
    total = description.total,
    rows = rows,
  }
end

return {
  ["view.table.describe"] = describe,
  ["view.table.page"] = page,
}

local ctx = ...
local max_assets = 6

local mime_types = {
  txt = "text/plain",
  log = "text/plain",
  md = "text/markdown",
  markdown = "text/markdown",
  html = "text/html",
  htm = "text/html",
  css = "text/css",
  png = "image/png",
  jpg = "image/jpeg",
  jpeg = "image/jpeg",
  gif = "image/gif",
  webp = "image/webp",
}

local function media_type(path)
  for i = #path, 1, -1 do
    local byte = string.byte(path, i)
    if byte == 46 then
      return mime_types[string.lower(string.sub(path, i + 1))] or "application/octet-stream"
    elseif byte == 47 then
      break
    end
  end
  return "application/octet-stream"
end

local function canonical_path(path)
  if type(path) ~= "string" then
    return nil
  end
  local length = utf8.len(path)
  if not length or length == 0 or length > 512 then
    return nil
  end
  local components = {}
  local start = 1
  for i = 1, #path + 1 do
    local byte = string.byte(path, i)
    if byte == 92 or (byte and (byte < 32 or byte == 127)) then
      return nil
    end
    if byte == 47 or not byte then
      local part = string.sub(path, start, i - 1)
      if part == "" or part == ".." or (not byte and part == ".") then
        return nil
      end
      if part ~= "." then
        components[#components + 1] = part
      end
      start = i + 1
    end
  end
  return table.concat(components, "/")
end

local function attachment_paths(text, main_path)
  local paths = {}
  if text == "" then
    return paths
  end
  if type(text) ~= "string" or #text > 12288 then
    return nil
  end
  local length = utf8.len(text)
  if not length or length > 3072 then
    return nil
  end

  local seen = { [main_path] = true }
  local parts = {}
  local i = 1
  while i <= #text + 1 do
    local byte = string.byte(text, i)
    if byte == 92 then
      if string.byte(text, i + 1) ~= 44 then
        return nil
      end
      parts[#parts + 1] = ","
      i = i + 1
    elseif byte == 44 or not byte then
      local path = canonical_path(table.concat(parts))
      if not path or seen[path] or #paths >= max_assets then
        return nil
      end
      seen[path] = true
      paths[#paths + 1] = path
      parts = {}
    else
      parts[#parts + 1] = string.sub(text, i, i)
    end
    i = i + 1
  end
  return paths
end

local function invalid(message)
  return nil, { code = "INVALID_INPUT", message = message, effect = "none" }
end

local function preview(args)
  if type(args) ~= "table" or math.type(args.maxBytes) ~= "integer" or
      args.maxBytes < 1 or args.maxBytes > 1048576 then
    return invalid("Preview requires a path and a valid byte limit")
  end
  local path = canonical_path(args.path)
  if not path then
    return invalid("Preview requires a path relative to the authorized workspace")
  end
  local attachments = attachment_paths(args.assets, path)
  if not attachments then
    return invalid("Preview attachments must be unique relative paths")
  end
  local mime = media_type(path)
  if #attachments > 0 and mime ~= "text/html" and mime ~= "text/markdown" then
    return invalid("Attachments require an HTML or Markdown document")
  end
  local types = {}
  for i = 1, #attachments do
    local kind = media_type(attachments[i])
    if kind ~= "text/css" and string.sub(kind, 1, 6) ~= "image/" then
      return invalid("Unsupported preview attachment type")
    end
    types[i] = kind
  end
  if not ctx.resources then
    return nil, { code = "UNSUPPORTED", message = "Resource publication unavailable", effect = "none" }
  end

  local resource, err = ctx.resources.publish("workspace", path, mime, args.maxBytes)
  if not resource then
    return nil, err
  end
  for i = 1, #attachments do
    local attachment, failure = ctx.resources.publish(
      "workspace", attachments[i], types[i], args.maxBytes
    )
    if not attachment then
      failure.effect = "possible"
      return nil, failure
    end
  end
  return resource
end

return {
  ["preview.open"] = preview,
}

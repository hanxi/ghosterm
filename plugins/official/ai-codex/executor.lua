local ctx = ...
if not ctx.process or not ctx.arguments then
  return nil
end

local version = "codex-cli 0.166.0"
local child, thread_id, turn_id
local starting_turn, announced_turn
local next_id = 0
local buffer = ""
local line_parts, line_bytes = {}, 0
local json_limit = 1048576
local snapshot_limit = 524288
local snapshot_budget = 1048576
-- Compact JSON at most doubles when quotes and backslashes are escaped for RPC.
local change_chunk_bytes = 1300
local discard_stderr_limit = 3072
local notifications = {}
local approvals = {}
local approval_count = 0
local event_sequence = 0
local event_handle
local changes, change_bytes = {}, 0
local change_sequence = 0

local function failed(code, message, effect)
  return nil, { code = code, message = message, effect = effect or "none" }
end

local function stop()
  thread_id, turn_id = nil, nil
  starting_turn, announced_turn = nil, nil
  approvals = {}
  approval_count = 0
  changes = {}
  change_bytes = 0
  if child then
    ctx.process.terminate(child)
  end
end

local function write(message)
  local encoded = ctx.json.encode(message)
  if not encoded then
    return false
  end
  local data = encoded .. "\n"
  local at = 1
  while at <= #data do
    local count = ctx.process.writeStdin(child, string.sub(data, at, at + 3071))
    if not count then
      return false
    elseif count == 0 then
      ctx.wait()
    else
      at = at + count
    end
  end
  return true
end

local function next_line()
  for at = 1, #buffer do
    if string.byte(buffer, at) == 10 then
      if line_bytes + at - 1 > json_limit then
        return nil, "invalid"
      end
      line_parts[#line_parts + 1] = buffer:sub(1, at - 1)
      buffer = buffer:sub(at + 1)
      local line = table.concat(line_parts)
      line_parts, line_bytes = {}, 0
      return line
    end
  end
  if #buffer > 0 then
    line_bytes = line_bytes + #buffer
    if line_bytes > json_limit then
      return nil, "invalid"
    end
    line_parts[#line_parts + 1] = buffer
    buffer = ""
  end
end

local function read_message()
  local line, invalid = next_line()
  if invalid then
    return nil, invalid
  end
  if not line then
    ctx.process.readStderr(child, discard_stderr_limit)
    local chunk, status = ctx.process.readStdout(child, 3072)
    if not chunk then
      return nil, status == "wait" and "wait" or "closed"
    end
    buffer = chunk
    line, invalid = next_line()
    if not line then
      return nil, invalid or "wait"
    end
  end
  local value = ctx.json.decode(line, json_limit)
  if type(value) ~= "table" then
    return nil, "invalid"
  end
  return value
end

local function queue_event(event)
  if #notifications >= 16 then
    return false
  end
  event.sequence = event_sequence + 1
  local body = ctx.json.encode(event)
  if not body or #body > 2048 then
    return false
  end
  event_sequence = event.sequence
  notifications[#notifications + 1] = body
  return true
end

local function request_key(id)
  if type(id) == "string" and #id > 0 and #id <= 128 then
    return "s:" .. id
  end
  local integer = type(id) == "number" and math.tointeger(id)
  if integer and integer >= -9007199254740991 and integer <= 9007199254740991 then
    return "n:" .. integer
  end
end

local function only_fields(object, fields)
  if type(object) ~= "table" then
    return false
  end
  for field in pairs(object) do
    if not fields[field] then
      return false
    end
  end
  return true
end

local function nul_free(value)
  if type(value) ~= "string" then
    return false
  end
  for at = 1, #value do
    if string.byte(value, at) == 0 then
      return false
    end
  end
  return true
end

local function permission_path(value)
  return nul_free(value) and #value > 0
end

local function valid_array(value, maximum, valid)
  if not ctx.json.isArray(value) or #value > maximum then
    return false
  end
  for _, element in ipairs(value) do
    if not valid(element) then
      return false
    end
  end
  return true
end

local function permission_entry(entry)
  if not only_fields(entry, { path = true, access = true }) or
      (entry.access ~= "read" and entry.access ~= "write" and entry.access ~= "deny") then
    return false
  end
  local path = entry.path
  if type(path) ~= "table" then
    return false
  elseif path.type == "path" then
    return only_fields(path, { type = true, path = true }) and permission_path(path.path)
  elseif path.type == "glob_pattern" then
    return only_fields(path, { type = true, pattern = true }) and permission_path(path.pattern)
  elseif path.type ~= "special" or not only_fields(path, { type = true, value = true }) then
    return false
  end
  local special = path.value
  if type(special) ~= "table" then
    return false
  elseif special.kind == "project_roots" then
    local subpath = special.subpath
    return only_fields(special, { kind = true, subpath = true }) and
      (subpath == nil or subpath == ctx.json.null or nul_free(subpath))
  end
  return only_fields(special, { kind = true }) and
    (special.kind == "root" or special.kind == "minimal" or special.kind == "tmpdir" or
      special.kind == "slash_tmp")
end

local function permission_grant(params)
  local profile = params.permissions
  if not nul_free(params.cwd) or
      not only_fields(profile, { network = true, fileSystem = true }) or
      profile.network == nil or profile.fileSystem == nil then
    return nil
  end
  local grant = {}
  local network, filesystem = profile.network, profile.fileSystem
  if network ~= ctx.json.null then
    if not only_fields(network, { enabled = true }) or
        (network.enabled ~= ctx.json.null and type(network.enabled) ~= "boolean") then
      return nil
    end
    grant.network = network
  end
  if filesystem ~= ctx.json.null then
    if not only_fields(filesystem, { read = true, write = true, globScanMaxDepth = true, entries = true }) then
      return nil
    end
    for _, field in ipairs({ "read", "write" }) do
      if filesystem[field] ~= ctx.json.null and not valid_array(filesystem[field], 64, permission_path) then
        return nil
      end
    end
    local depth = filesystem.globScanMaxDepth
    if depth ~= nil and (math.type(depth) ~= "integer" or depth < 1 or depth > 9007199254740991) then
      return nil
    end
    if filesystem.entries ~= nil and not valid_array(filesystem.entries, 64, permission_entry) then
      return nil
    end
    grant.fileSystem = filesystem
  end
  return grant
end

local function capture_change(params, item)
  local item_id = item == params and params.itemId or item.id
  if params.threadId ~= thread_id or not permission_path(params.turnId) or #params.turnId > 128 or
      not permission_path(item_id) or #item_id > 128 then
    return false
  end
  local function valid(change)
    return type(change) == "table" and permission_path(change.path) and
      type(change.diff) == "string" and type(change.kind) == "table" and
      (change.kind.type == "add" or change.kind.type == "delete" or change.kind.type == "update")
  end
  if not valid_array(item.changes, 4096, valid) then
    return false
  end
  local key = params.turnId .. "\0" .. item_id
  local content = ctx.json.encode({ schemaVersion = 1, threadId = thread_id,
    turnId = params.turnId, itemId = item_id, changes = item.changes }, snapshot_limit)
  local snapshot = changes[key]
  if not content then
    change_bytes = change_bytes - (snapshot and #snapshot.content or 0)
    changes[key] = nil
    item._ghosterm = { snapshotUnavailable = "limit" }
  else
    if not snapshot or snapshot.content ~= content then
      local units, anchor = 0, 1
      local index = { { byte = 1, units = 0 } }
      for byte, code in utf8.codes(content) do
        if byte - anchor >= 400 then
          anchor = byte
          index[#index + 1] = { byte = byte, units = units }
        end
        units = units + (code > 0xffff and 2 or 1)
      end
      changes[key] = nil
      change_bytes = change_bytes - (snapshot and #snapshot.content or 0)
      while true do
        local count, oldest = 0, nil
        for candidate, retained in pairs(changes) do
          count = count + 1
          if not oldest or retained.sequence < changes[oldest].sequence then
            oldest = candidate
          end
        end
        if count < 16 and change_bytes + #content <= snapshot_budget then
          break
        end
        change_bytes = change_bytes - #changes[oldest].content
        changes[oldest] = nil
      end
      change_sequence = change_sequence + 1
      snapshot = { id = "change-" .. change_sequence, sequence = change_sequence,
        content = content, units = units, index = index }
      changes[key] = snapshot
      change_bytes = change_bytes + #content
    end
    item._ghosterm = { snapshotID = snapshot.id, snapshotUnits = snapshot.units,
      snapshotBytes = #content }
  end
  item._ghosterm.omittedChanges = #item.changes
  item._ghosterm.truncated = true
  item._ghosterm.diffTruncated = true
  item.changes = ctx.json.array()
  return true
end

local function read_change(input)
  if type(input) ~= "table" or type(input.snapshotID) ~= "string" or #input.snapshotID > 128 then
    return failed("INVALID_INPUT", "Invalid Codex patch reference")
  end
  local offset, snapshot = input.offset, nil
  for _, candidate in pairs(changes) do
    if candidate.id == input.snapshotID then
      snapshot = candidate
      break
    end
  end
  local value = { status = snapshot and "invalid" or "expired", snapshotID = input.snapshotID,
    offset = 0, nextOffset = 0, totalUnits = 0, totalBytes = 0, content = "", done = false }
  if not snapshot or math.type(offset) ~= "integer" or offset < 0 or offset > snapshot.units then
    return value
  end
  local low, high = 1, #snapshot.index
  while low < high do
    local middle = (low + high + 1) // 2
    if snapshot.index[middle].units > offset then
      high = middle - 1
    else
      low = middle
    end
  end
  local anchor = snapshot.index[low]
  local units, start_byte, end_byte = anchor.units, nil, #snapshot.content + 1
  local tail = snapshot.content:sub(anchor.byte, anchor.byte + change_chunk_bytes + 410)
  -- Only iterate complete UTF-8 scalars, including at the end of this window.
  local last = #tail
  if anchor.byte + last - 1 < #snapshot.content then
    while string.byte(snapshot.content, anchor.byte + last) & 0xc0 == 0x80 do
      last = last - 1
    end
    tail = tail:sub(1, last)
  end
  for relative, code in utf8.codes(tail) do
    local at = anchor.byte + relative - 1
    if units == offset then
      start_byte = at
    end
    if not start_byte and units > offset then
      return value
    elseif start_byte and at - start_byte > change_chunk_bytes then
      end_byte = at
      break
    end
    units = units + (code > 0xffff and 2 or 1)
  end
  if offset == snapshot.units then
    start_byte = end_byte
  elseif not start_byte then
    return value
  end
  return { status = "ok", snapshotID = snapshot.id, offset = offset, nextOffset = units,
    totalUnits = snapshot.units, totalBytes = #snapshot.content,
    content = snapshot.content:sub(start_byte, end_byte - 1), done = units == snapshot.units }
end

local function server_message(message)
  local method, params = message.method, message.params
  if type(method) ~= "string" then
    return false
  end
  if message.id ~= nil then
    local key = request_key(message.id)
    local permission = method == "item/permissions/requestApproval"
    local file = method == "item/fileChange/requestApproval"
    if (not permission and not file and method ~= "item/commandExecution/requestApproval") or not key or approvals[key] or
        approval_count >= 16 or type(params) ~= "table" or not turn_id or
        params.threadId ~= thread_id or params.turnId ~= turn_id or
        not permission_path(params.itemId) or #params.itemId > 128 then
      return false
    end
    local grant = permission and permission_grant(params)
    if permission and not grant then
      return false
    end
    local review = permission and ("permissions." .. (event_sequence + 1)) or nil
    if not queue_event({ kind = "approval-request", request = {
      id = message.id, method = method, params = params, permissionReviewID = review,
    } }) then
      return false
    end
    approvals[key] = { id = message.id, choices = params.availableDecisions, review = review, grant = grant,
      change = file and (params.turnId .. "\0" .. params.itemId) or nil }
    approval_count = approval_count + 1
    return true
  end
  if method == "turn/started" and starting_turn then
    if type(params) ~= "table" or params.threadId ~= thread_id or
        type(params.turn) ~= "table" or not permission_path(params.turn.id) or
        #params.turn.id > 128 or announced_turn then
      return false
    end
    announced_turn = params.turn.id
    turn_id = announced_turn
  elseif method == "turn/completed" and type(params) == "table" and params.threadId == thread_id and
      type(params.turn) == "table" and params.turn.id == turn_id then
    turn_id = nil
    approvals = {}
    approval_count = 0
  elseif method == "serverRequest/resolved" and type(params) == "table" and
      params.threadId == thread_id then
    local key = request_key(params.requestId)
    if key and approvals[key] then
      approvals[key] = nil
      approval_count = approval_count - 1
      return queue_event({ kind = "approval-resolved", requestID = params.requestId })
    end
  end
  if type(params) == "table" then
    local item = params.item
    if method == "item/fileChange/patchUpdated" then
      if not capture_change(params, params) then
        return false
      end
    elseif (method == "item/started" or method == "item/completed") and
        type(item) == "table" and item.type == "fileChange" and not capture_change(params, item) then
      return false
    end
  end
  return queue_event({ kind = "notification",
    notification = { method = method, params = params or ctx.json.null },
  })
end

local function request(method, params)
  next_id = next_id + 1
  local id = next_id
  if not write({ id = id, method = method, params = params }) then
    stop()
    return failed("IO", "Codex request could not be written", "possible")
  end
  while true do
    local message, status = read_message()
    if not message and status == "wait" then
      ctx.wait()
    elseif not message then
      stop()
      return failed("IO", "Codex reply was not confirmed", "possible")
    elseif message.id == id and message.method == nil then
      if message.error ~= nil or type(message.result) ~= "table" then
        return failed("PROTOCOL", "Codex rejected the request", "possible")
      end
      return message.result
    elseif not server_message(message) then
      stop()
      return failed("UNSUPPORTED", "Unsupported Codex server message", "possible")
    end
  end
end

local function start_thread()
  if child then
    return failed("BUSY", "Codex thread already started")
  end
  local mapped_version
  child, mapped_version = ctx.process.spawn("codex", "workspace", { "app-server" }, version)
  if not child then
    if mapped_version and mapped_version.code == "UNSUPPORTED" then
      return failed("UNSUPPORTED", "Codex app-server version does not match the protocol lock")
    end
    return failed("IO", "Codex CLI could not be started")
  end
  if mapped_version ~= version then
    stop()
    return failed("UNSUPPORTED", "Codex app-server version does not match the protocol lock")
  end
  local initialized, err = request("initialize", {
    clientInfo = { name = "ghosterm", title = "Ghosterm Codex", version = "0.1.0" },
    capabilities = { experimentalApi = false },
  })
  if not initialized then
    return nil, err
  end
  if not write({ method = "initialized", params = {} }) then
    stop()
    return failed("IO", "Codex initialization was not confirmed", "possible")
  end
  local params = { approvalPolicy = "on-request", approvalsReviewer = "user" }
  if ctx.arguments.ephemeral then
    params.ephemeral = true
  end
  if ctx.arguments.readOnly then
    params.sandbox = "read-only"
  end
  local result, failure = request("thread/start", params)
  if not result then
    return nil, failure
  end
  if type(result.thread) ~= "table" or type(result.thread.id) ~= "string" or
      #result.thread.id == 0 or #result.thread.id > 128 then
    stop()
    return failed("PROTOCOL", "Codex replied without a thread ID", "possible")
  end
  local sandbox = type(result.sandbox) == "table" and result.sandbox.type
  if result.approvalPolicy ~= params.approvalPolicy or
      result.approvalsReviewer ~= params.approvalsReviewer or
      (sandbox ~= "readOnly" and sandbox ~= "workspaceWrite") or
      (ctx.arguments.readOnly and sandbox ~= "readOnly") then
    stop()
    return failed("PROTOCOL", "Codex did not confirm the expected sandbox and approval policy", "possible")
  end
  if not queue_event({
    kind = "notification",
    notification = {
      method = "ghosterm/threadConfigured",
      params = {
        threadId = result.thread.id,
        approvalPolicy = result.approvalPolicy,
        approvalsReviewer = result.approvalsReviewer,
        sandboxType = sandbox,
      },
    },
  }) then
    stop()
    return failed("PROTOCOL", "Codex thread policy observation could not be queued", "possible")
  end
  thread_id = result.thread.id
  return { threadID = thread_id }
end

local function send(input)
  local length = type(input) == "table" and type(input.text) == "string" and utf8.len(input.text)
  if not thread_id or turn_id or not length or length == 0 or length > 2048 then
    return failed("INVALID_INPUT", "Codex turn is unavailable or input is invalid")
  end
  starting_turn = true
  local result, err = request("turn/start", {
    threadId = thread_id,
    input = { { type = "text", text = input.text, text_elements = ctx.json.array() } },
  })
  local announced = announced_turn
  starting_turn, announced_turn = nil, nil
  if not result then
    if announced then
      stop()
    end
    return nil, err
  end
  if type(result.turn) ~= "table" or type(result.turn.id) ~= "string" or
      #result.turn.id == 0 or #result.turn.id > 128 or (announced and announced ~= result.turn.id) then
    stop()
    return failed("PROTOCOL", "Codex replied without a turn ID", "possible")
  end
  if not announced then
    turn_id = result.turn.id
  end
  return { turnID = result.turn.id }
end

local function interrupt()
  if not thread_id or not turn_id then
    return failed("INVALID_INPUT", "No active Codex turn")
  end
  local result, err = request("turn/interrupt", { threadId = thread_id, turnId = turn_id })
  if not result then
    return nil, err
  end
  return { accepted = true }
end

local function respond_approval(input)
  local key = type(input) == "table" and input.requestID
  local approval = type(key) == "string" and approvals[key]
  local decision = approval and type(input.decisionJSON) == "string" and
    ctx.json.decode(input.decisionJSON)
  if not thread_id or not turn_id or (decision ~= "accept" and decision ~= "decline") then
    return failed("INVALID_INPUT", "Codex approval request or decision is unavailable")
  end
  local result = { decision = decision }
  if approval.review then
    if input.snapshotID ~= approval.review then
      return failed("DENIED", "Codex permission review no longer matches the request")
    end
    result = { permissions = decision == "accept" and approval.grant or {}, scope = "turn" }
  elseif approval.change then
    local current = changes[approval.change]
    if decision == "accept" and (not current or current.id ~= input.snapshotID) then
      return { accepted = false }
    end
  elseif input.snapshotID ~= "" then
    return failed("INVALID_INPUT", "Codex command approval does not use a review snapshot")
  elseif approval.choices ~= nil and approval.choices ~= ctx.json.null then
    local offered = false
    if type(approval.choices) == "table" then
      for _, choice in ipairs(approval.choices) do
        offered = offered or choice == decision
      end
    end
    if not offered then
      return failed("DENIED", "Codex did not offer this approval decision")
    end
  end
  approvals[key] = nil
  approval_count = approval_count - 1
  if not write({ id = approval.id, result = result }) then
    stop()
    return failed("IO", "Codex approval reply was not confirmed", "possible")
  end
  return { accepted = true }
end

ctx.onStep(function()
  if event_handle then
    local value, err = ctx.rpc.poll(event_handle)
    if value or err then
      event_handle = nil
      if not value or value.accepted ~= true then
        stop()
        notifications = {}
      end
    end
    return
  end
  if #notifications == 0 and thread_id then
    local message, status = read_message()
    if message and not server_message(message) then
      stop()
      queue_event({ kind = "protocol-error", message = "Unsupported Codex server message" })
    elseif not message and status ~= "wait" then
      stop()
      queue_event({ kind = "outcome-unknown", message = "Codex app-server stopped responding" })
    end
  end
  if #notifications > 0 then
    event_handle = ctx.rpc.start("codex.event", { event = notifications[1] }, 5000)
    if event_handle then
      table.remove(notifications, 1)
    else
      stop()
      notifications = {}
    end
  end
end)

return {
  ["codex.thread.start"] = start_thread,
  ["codex.turn.send"] = send,
  ["codex.turn.interrupt"] = interrupt,
  ["codex.approval.respond"] = respond_approval,
  ["codex.change.read"] = read_change,
}

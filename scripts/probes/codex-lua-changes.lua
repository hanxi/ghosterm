local path, name = ...
local incoming, writes, events, records = {}, {}, {}, {}
local step
local terminated = false
local function encode(value, limit)
  if value.schemaVersion then
    local content = value.changes[1].diff
    return #content <= limit and content or nil
  end
  records[#records + 1] = value
  return tostring(#records)
end

local ctx = {
  arguments = {},
  onStep = function(fn) step = fn end,
  wait = function() error("unexpected fixture wait") end,
  json = {
    encode = encode,
    decode = function(text)
      if text == '"accept"' then return "accept" end
      return records[tonumber(text)]
    end,
    isArray = function(value) return type(value) == "table" and value.marked == true end,
    array = function() return { marked = true } end,
  },
  process = {
    spawn = function() return 1, "codex-cli 0.166.0" end,
    terminate = function()
      assert(name == "thread-policy", "unexpected fixture shutdown")
      terminated = true
    end,
    readStderr = function() end,
    readStdout = function()
      local message = table.remove(incoming, 1)
      if message then return encode(message) .. "\n" end
      return nil, "wait"
    end,
    writeStdin = function(_, data)
      writes[#writes + 1] = records[tonumber(data)]
      return #data
    end,
  },
  rpc = {
    start = function(method, input)
      assert(method == "codex.event")
      events[#events + 1] = records[tonumber(input.event)]
      return 1
    end,
    poll = function() return { accepted = true } end,
  },
}

local methods = assert(loadfile(path))(ctx)
incoming = {
  { id = 1, result = {} },
  { id = 2, result = { thread = { id = "thread" }, sandbox = { type = "readOnly" },
    approvalPolicy = "on-request", approvalsReviewer = name == "thread-policy" and "auto_review" or "user" } },
}
local thread, err = methods["codex.thread.start"]()
if name == "thread-policy" then
  assert(thread == nil and err.code == "PROTOCOL" and terminated and #events == 0,
    "CODEX CHANGE ASSERTION " .. name)
  print("CODEX CHANGE CASE PASS " .. name)
  return
end
assert(thread.threadID == "thread")
assert(writes[3].method == "thread/start" and writes[3].params.approvalPolicy == "on-request")
step()
step()
local configured = assert(events[1]).notification
assert(configured.method == "ghosterm/threadConfigured" and configured.params.sandboxType == "readOnly")
if name == "control" then
  print("CODEX CHANGE CONTROL PASS")
  return
end
incoming = { { id = 3, result = { turn = { id = "turn" } } } }
assert(methods["codex.turn.send"]({ text = "fixture" }).turnID == "turn")

local function deliver(message)
  incoming = { message }
  step()
  step()
  return events[#events]
end

local function patch(item, content)
  return deliver({ method = "item/fileChange/patchUpdated", params = {
    threadId = "thread", turnId = "turn", itemId = item,
    changes = { marked = true, { path = "fixture.txt", kind = { type = "update" }, diff = content } },
  } }).notification.params._ghosterm
end

local function approval(snapshot)
  return methods["codex.approval.respond"]({
    requestID = "n:17", snapshotID = snapshot, decisionJSON = '"accept"',
  })
end

local function request_approval()
  deliver({ id = 17, method = "item/fileChange/requestApproval",
    params = { threadId = "thread", turnId = "turn", itemId = "reviewed" } })
end

local function require_case(condition)
  assert(condition, "CODEX CHANGE ASSERTION " .. name)
end

if name == "identity" then
  patch("reviewed", "A")
  local result = methods["codex.change.read"]({ snapshotID = "missing", offset = 0 })
  require_case(result.status == "expired")
elseif name == "size" then
  local marker = patch("reviewed", string.rep("x", 524289))
  require_case(marker.snapshotUnavailable == "limit" and marker.snapshotID == nil)
elseif name == "wire" then
  local marker = patch("reviewed", string.rep(string.char(92), 5000))
  local chunk = methods["codex.change.read"]({ snapshotID = marker.snapshotID, offset = 0 })
  -- Every byte in this fixture needs two bytes in the escaped RPC string.
  require_case(chunk.status == "ok" and chunk.nextOffset > 0 and #chunk.content * 2 + 200 <= 3072)
elseif name == "approval-scope" or name == "approval-revision" then
  local old = patch("reviewed", "first").snapshotID
  local unrelated = patch("other", "unrelated").snapshotID
  request_approval()
  local current = patch("reviewed", "updated").snapshotID
  local count = #writes
  local rejected = approval(name == "approval-scope" and unrelated or old)
  require_case(rejected and rejected.accepted == false and #writes == count)
  require_case(approval(current).accepted == true and writes[#writes].id == 17)
else
  error("unknown fixture case")
end
print("CODEX CHANGE CASE PASS " .. name)

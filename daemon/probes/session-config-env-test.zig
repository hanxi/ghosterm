// Injected into isolated Config.zig by probe-session-config-env.py.
// Synthetic HOME belongs to the test child process only.


test "p0-config-env selected allocator failure propagates" {
    var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = 0 });
    var buffer: [4096]u8 = undefined;
    try std.testing.expectError(error.OutOfMemory, expandHome(failing.allocator(), "~/ghosterm", &buffer));
    try std.testing.expect(failing.has_induced_failure);
}

test "p0-config-env absolute control does not allocate" {
    var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = 0 });
    var buffer: [1]u8 = undefined;
    try std.testing.expectEqualStrings("/ghosterm/absolute", try expandHome(failing.allocator(), "/ghosterm/absolute", &buffer));
    try std.testing.expect(!failing.has_induced_failure);
}

test "p0-config-env home expansion and short buffer" {
    var buffer: [4096]u8 = undefined;
    const expanded = try expandHome(std.testing.allocator, "~/ghosterm", &buffer);
    // Test runner supplies this synthetic HOME to this process only.
    try std.testing.expectEqualStrings("/ghosterm-p0-config-home/ghosterm", expanded);
    var small: [1]u8 = undefined;
    try std.testing.expectError(error.HomeDetectionFailed, expandHome(std.testing.allocator, "~/ghosterm", &small));
}


test "p0-config-env working directory failure and control" {
    var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = 0 });
    var value: WorkingDirectory = .{ .path = "~/ghosterm" };
    try std.testing.expectError(error.OutOfMemory, value.finalize(failing.allocator()));
    try std.testing.expect(failing.has_induced_failure);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try value.finalize(arena.allocator());
    try std.testing.expectEqualStrings("/ghosterm-p0-config-home/ghosterm", value.path);
}

test "p0-config-env light theme failure and control" {
    var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = 0 });
    var value: Theme = .{ .light = "~/light", .dark = "/dark" };
    try std.testing.expectError(error.OutOfMemory, value.finalize(failing.allocator()));
    try std.testing.expect(failing.has_induced_failure);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try value.finalize(arena.allocator());
    try std.testing.expectEqualStrings("/ghosterm-p0-config-home/light", value.light);
    try std.testing.expectEqualStrings("/dark", value.dark);
}

test "p0-config-env dark theme failure and control" {
    var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = 0 });
    var value: Theme = .{ .light = "/light", .dark = "~/dark" };
    try std.testing.expectError(error.OutOfMemory, value.finalize(failing.allocator()));
    try std.testing.expect(failing.has_induced_failure);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try value.finalize(arena.allocator());
    try std.testing.expectEqualStrings("/light", value.light);
    try std.testing.expectEqualStrings("/ghosterm-p0-config-home/dark", value.dark);
}

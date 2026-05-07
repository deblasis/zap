const std = @import("std");
const zap = @import("zap");

fn on_request(r: zap.Request) !void {
    try r.sendBody("<html><body><h1>Hello from ZAPPA!!!</h1></body></html>");
}

pub fn main() !void {
    var listener = zap.HttpListener.init(.{
        .port = 19091,
        .on_request = on_request,
        .log = false,
        .max_clients = 100000,
    });
    try listener.listen();
    std.debug.print("Zap bench server on :19091\n", .{});
    zap.start(.{
        .threads = 1,
        .workers = 1,
    });
}

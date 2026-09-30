// BenchBoard 压测用「快目标」—— 就是为了把 k6 的 JSON 输出拉到万级 rps
//
// 为什么需要它：
//   python -m http.server 单线程，8 VU 也就 ~1000 rps（docs/02 第 3.10 节基线）。
//   要复现"界面被火管淹掉"的场景，目标必须能扛到 10k rps 量级 —— 也就是
//   每秒往 stdout 灌 14 万个 JSON 行。
//
// 用法：
//   node tools/probes/fast_http_server.js [port]
//   默认 8901
//
// 只用 Node 内置 http 模块，keep-alive 默认开启，零依赖。

const http = require('http');

const port = parseInt(process.argv[2] || '8901', 10);

const server = http.createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/plain', 'Content-Length': '2' });
  res.end('ok');
});

// keep-alive 拉满，减少 TIME_WAIT 和握手开销
server.keepAliveTimeout = 60000;
server.headersTimeout = 65000;
server.maxRequestsPerSocket = 0;

server.listen(port, '127.0.0.1', () => {
  console.log('fast target listening on http://127.0.0.1:' + port + '/');
});
